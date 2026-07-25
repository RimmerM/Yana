#pragma once

#include "Net/Buffer.h"
#include "Net/Stream.h"
#include "../../lower/lower_inst.h"

static constexpr Size kMaxRegInputs = 16;

enum RegClass {
    GenReg,
    XmmReg,
    StackReg,
    RematReg,
};

using RegId = I16;

// The classes that name a physical machine register. StackReg and RematReg are location classes but
// not physical ones - a frame slot is not part of the register file, and a rematerializable value
// occupies nothing at all - so both are excluded from everything that describes what an operation
// does to the machine.
static constexpr Size kPhysRegClassCount = 2;
static constexpr Size kRegClassCount = 4;
static constexpr RegId kInvalidReg = 0x7fff;

inline RegClass getRegClass(RegId id) {
    return RegClass(id >> 12);
}

inline U16 getRegIndex(RegId id) {
    return id & 0x0fff;
}

inline RegId makeRegId(RegClass c, U16 index) {
    return (I16(c) << 12) | I16(index);
}

inline RegClass classForType(LowerType type) {
    return isIntLike(type) ? GenReg : XmmReg;
}

// A set of physical registers, one bitmask per class. Everything that describes what a machine
// operation does to the register file is one of these: an instruction's clobbers, a calling
// convention's preserved set, the registers a value has to stay out of while it is live.
//
// Being per class is what lets a convention state that a call destroys xmm0-15 as well as the
// caller-saved integer registers. A single mask could only ever describe one class, which is why no
// convention could describe a vector clobber before.
//
// A location that is not a physical register - a stack slot, a rematerializable value, or
// kInvalidReg - is never a member, and adding one is a no-op rather than an error. That is what
// makes it safe to feed an operand's location straight in without first asking whether the allocator
// gave it a register at all.
struct RegSet {
    U64 classes[kPhysRegClassCount] = {};

    static bool isPhysical(RegId id) {
        return Size(getRegClass(id)) < kPhysRegClassCount;
    }

    bool has(RegId id) const {
        return isPhysical(id) && (classes[getRegClass(id)] & (U64(1) << getRegIndex(id))) != 0;
    }

    void add(RegId id) {
        if(isPhysical(id)) classes[getRegClass(id)] |= U64(1) << getRegIndex(id);
    }

    void remove(RegId id) {
        if(isPhysical(id)) classes[getRegClass(id)] &= ~(U64(1) << getRegIndex(id));
    }

    bool isEmpty() const {
        for(auto c: classes) {
            if(c) return false;
        }

        return true;
    }

    RegSet& operator |= (const RegSet& other) {
        for(Size i = 0; i < kPhysRegClassCount; i++) classes[i] |= other.classes[i];
        return *this;
    }

    RegSet operator | (const RegSet& other) const {
        auto set = *this;
        set |= other;
        return set;
    }

    RegSet operator & (const RegSet& other) const {
        RegSet set;
        for(Size i = 0; i < kPhysRegClassCount; i++) set.classes[i] = classes[i] & other.classes[i];
        return set;
    }

    // The registers of `within` that this set does not contain. A convention's preserved set is
    // exactly this applied to its clobber set: a register a call leaves alone is one its callee
    // owes back.
    RegSet complement(const RegSet& within) const {
        RegSet set;
        for(Size i = 0; i < kPhysRegClassCount; i++) set.classes[i] = ~classes[i] & within.classes[i];
        return set;
    }

    bool operator == (const RegSet& other) const {
        for(Size i = 0; i < kPhysRegClassCount; i++) {
            if(classes[i] != other.classes[i]) return false;
        }

        return true;
    }
};

struct ClassConstraints {
    // Maps instruction argument index -> register id.
    RegId args[kMaxRegInputs];
    RegId results[kMaxRegInputs];
};

// The fixed-register behaviour of one instruction: which of its operands and results have to occupy
// a particular register, and which registers it destroys along the way. Only instructions whose
// encoding forces the issue have an entry - a division's rax/rdx protocol, a shift's count in rcx,
// the string operations' rdi/rsi/rcx. Everything else takes whatever the allocator hands it.
//
// A calling convention is described by CallConvention below rather than here: a call's operand
// locations depend on how many arguments of each class came before, which a flat table cannot say.
struct InstConstraints {
    InstConstraints();

    // Argument and result constraints.
    ClassConstraints constraints[kPhysRegClassCount];

    // Registers the instruction writes behind its operands' backs.
    RegSet clobber;
};

// Where one argument or result of a call is passed.
//
// This is the single place the register-versus-stack decision is made. The caller writes a stack
// argument into its outgoing area at `stackOffset`, the callee reads it from its incoming area at
// the same offset, and both get the answer from classifyArgs rather than deciding for themselves -
// which is what stops the two sides from disagreeing about where an argument went.
struct ArgLocation {
    enum Kind: U8 {
        None,     // unconstrained: the operand stays wherever the allocator put it
        Register, // a fixed register
        Stack,    // the argument area, at `stackOffset` bytes from its base
    };

    Kind kind = None;
    RegId reg = kInvalidReg;
    U32 stackOffset = 0;

    static ArgLocation inRegister(RegId reg) { return ArgLocation { Register, reg, 0 }; }
    static ArgLocation onStack(U32 offset) { return ArgLocation { Stack, kInvalidReg, offset }; }
};

// A calling convention, stated once and used from both sides.
//
// InstConstraints above describes what a fixed-register *instruction* does. This describes what a
// *call* does where it appears and what a function compiled with the convention owes the caller it
// returns to - the same contract seen from opposite ends, which is why constraint.cpp states both
// halves and checks them against each other rather than deriving either.
struct CallConvention {
    // The registers arguments and results are assigned to, per class, in the order the convention
    // hands them out. An argument that runs past the end of its class's list is passed in the
    // argument area instead; a result that does would have to be returned through memory, which
    // needs a hidden pointer argument the lowering does not produce, so classifyResults rejects it.
    struct ClassRegs {
        RegId regs[kMaxRegInputs];
        Size count = 0;

        void add(RegId reg) {
            assertTrue(count < kMaxRegInputs);
            regs[count++] = reg;
        }
    };

    ClassRegs args[kPhysRegClassCount];
    ClassRegs results[kPhysRegClassCount];

    // What a call of this convention destroys, and what a function compiled with it has to give
    // back. rsp is preserved too, but it is reserved rather than handed out, so keeping it valid is
    // the frame code's business rather than the prologue's. rbp is in neither position: no
    // convention may clobber it (a caller may be holding a frame pointer there), so it is preserved
    // by every one of them and saved by the prologue of any function that takes it as a register.
    RegSet clobber;
    RegSet calleeSaved;

    // What rsp must be a multiple of at the point a call of this convention is executed, before the
    // call pushes its return address. A convention the compiler is on both sides of can leave this
    // at 8; an external one generally cannot, because its callees are entitled to assume the
    // alignment when they spill a vector register.
    U32 stackAlignment = 8;

    // Bytes the caller reserves below the first stack argument, for a convention that requires the
    // callee to have somewhere to spill its register arguments - Win64's shadow space. Zero
    // everywhere else.
    U32 shadowSpace = 0;

    // Win64 assigns argument registers by position rather than per class: a float in argument
    // position 2 takes xmm2 and leaves r8 unused, so a callee can find any argument without knowing
    // the types of the ones before it. SysV and the compiler's own conventions count each class
    // independently, filling rdi..r9 with integers however many floats came first.
    bool positionalArgs = false;

    // Set once the convention has tables to work from. A function or call using one that does not
    // has to be rejected: with empty tables every argument would silently be classified onto the
    // stack, which is a working compile of the wrong program.
    bool defined = false;
};

// Assigns every argument of a call its location, by walking the argument list in order and handing
// out registers of each class until the convention runs out. Both sides go through this - the caller
// to place its operands, the callee to find where its arguments arrived, the verifier to check both.
//
// `typeOf` is asked for the type of argument `i`, so a caller can classify straight out of whatever
// buffer it already has without building a type list first.
template<class F>
void classifyArgs(const CallConvention& convention, Size count, F&& typeOf, Array<ArgLocation>& out) {
    assertTrue(convention.defined); // a call using an undescribed convention

    Size taken[kPhysRegClassCount] = {};
    auto stack = convention.shadowSpace;

    for(Size i = 0; i < count; i++) {
        auto cls = classForType(typeOf(i));
        auto& table = convention.args[cls];

        // A positional convention indexes the table by argument position, so an argument of one
        // class consumes the slot of every class; a per-class one keeps an independent counter each.
        auto index = convention.positionalArgs ? i : taken[cls];

        if(index < table.count) {
            out.push(ArgLocation::inRegister(table.regs[index]));
            taken[cls]++;
        } else {
            // Every stack argument occupies one 8-byte slot, in declaration order and lowest first,
            // which is what the callee's incoming offsets assume.
            out.push(ArgLocation::onStack(stack));
            stack += 8;
        }
    }
}

// The same for a call's results, which no described convention passes on the stack.
template<class F>
void classifyResults(const CallConvention& convention, Size count, F&& typeOf, Array<ArgLocation>& out) {
    assertTrue(convention.defined); // a call using an undescribed convention

    Size taken[kPhysRegClassCount] = {};

    for(Size i = 0; i < count; i++) {
        auto cls = classForType(typeOf(i));
        auto& table = convention.results[cls];
        auto index = taken[cls]++;

        assertTrue(index < table.count); // more results than the calling convention can return
        out.push(ArgLocation::inRegister(table.regs[index]));
    }
}

// Bytes of argument area a call needs: enough for the highest stack argument, plus any shadow space
// the convention asks for even when every argument fitted in a register, rounded up so that opening
// the area cannot knock rsp off the boundary the callee is entitled to expect.
U32 argAreaBytes(const CallConvention& convention, const Array<ArgLocation>& args);

// Byte count above which a Copy/SetPattern with a compile-time size stops being straight-lined into
// plain moves and takes the rep-prefixed string instruction instead. Chosen once, in
// transformFunction (see selectBlockOpEncoding), and recorded on the instruction.
static constexpr U64 kMaxUnrolledMemOp = 32;

struct Constraints {
    Constraints();
    const InstConstraints* getConstraints(LowerBase base, LowerInst* inst) const;
    const CallConvention& getConvention(LowerCallType type) const;

private:
    // Copy/SetPattern have two encodings with very different register requirements: `rep movsb`/
    // `rep stosb` (movsb/stosb) demand fixed registers and consume them, while the unrolled-mov
    // form (movsbImm/stosbImm) works out of whatever registers the operands already occupy. Which
    // one applies is decided once by transformFunction and read back off the instruction, so this
    // table and genCopy/genSetPattern cannot disagree about it.
    InstConstraints mul, div, rem, shift, movsb, stosb, movsbImm, stosbImm;
    CallConvention convention[(Size)LowerCallType::LastType + 1];
};

// The target's tables, built once. They are constant and the same for every function, and each of
// the three passes that reads them used to construct its own copy.
const Constraints& targetConstraints();

/*
 * Instruction shapes.
 *
 * Where each operand and result of one instruction has to be is worked out once, into an InstShape,
 * and then read back by index. The tables it comes from are indexed per register class and skip
 * operands that occupy no register at all, so "operand N of this instruction" and "entry N of the
 * table" are not the same thing - and every caller used to re-derive that mapping with its own copy
 * of the counting rule, which is how the allocator and the verifier could disagree about it.
 */

struct InstShape {
    // Parallel to the instruction's own used()/created() buffers, so operand N is entry N.
    Array<ArgLocation> uses;
    Array<ArgLocation> creates;

    // Registers the instruction writes behind its operands' backs.
    RegSet clobber;

    // The convention a call, a syscall or a return follows, for the callers that need more of it
    // than the operand locations: the argument area's size and alignment, and the preserved set.
    const CallConvention* convention = nullptr;

    // A return's operands are constrained like its convention's *results* rather than its
    // arguments, and nothing is live once the function has returned - so a return neither clobbers
    // anything nor has anything left to protect.
    bool isReturn = false;
};

InstShape shapeOf(LowerBase base, const Constraints& constraints, LowerFunction& fun, LowerInst* inst);

// The fixed register operand `i` has to be in when the instruction executes, if any. A stack-passed
// argument has no register and answers kInvalidReg here, so a caller that needs to tell the two
// apart reads shape.uses[i] instead.
inline RegId wantForUse(const InstShape& shape, Size i) {
    auto& location = shape.uses[i];
    return location.kind == ArgLocation::Register ? location.reg : kInvalidReg;
}

// The fixed register result `i` is produced in, if any.
inline RegId wantForResult(const InstShape& shape, Size i) {
    auto& location = shape.creates[i];
    return location.kind == ArgLocation::Register ? location.reg : kInvalidReg;
}

// Every register this instruction writes behind the operands' backs: the ones it clobbers, plus the
// ones the parallel copy in front of it writes to satisfy fixed-register constraints. A value that
// has to survive the instruction, and an operand that isn't itself placed by that parallel copy,
// both have to stay out of these.
RegSet writtenRegisters(const InstShape& shape);

/*
 * Memory operands.
 *
 * A value that lives in the frame normally has to be brought into a register before anything can
 * read it. Most x86 ALU instructions have a form that reads one operand straight out of memory
 * instead, which removes the reload entirely - `add rax, [slot]` in place of a load and an add.
 *
 * Which operand that is, if any, is one table rather than a decision made twice: the allocator asks
 * it to know whether to leave a spilled operand where it is, the encoder emits the memory form when
 * it finds a slot there, and the verifier asks the same question to catch the case where a slot
 * reaches an encoder that has no form for it.
 */

// Returned by memoryUseOperand for an instruction that has to have every operand in a register.
static constexpr I32 kNoMemoryOperand = -1;

// The one operand of `inst` that may be read directly out of a frame slot, as an index into its
// used() buffer, or kNoMemoryOperand.
//
// At most one, which is both what the encodings allow (a general memory operand occupies the r/m
// field, and there is one of those) and what keeps the answer a single index.
I32 memoryUseOperand(LowerBase base, LowerInst* inst);

// The one operand of `inst` that may be read *and written* in place, as an index into its used()
// buffer, or kNoMemoryOperand. Answers only for the destructive two-address encodings whose r/m
// operand is the destination - `add r/m, r` rather than `add r, r/m` - so it is always operand zero
// where it answers at all.
//
// Unlike memoryUseOperand this is not enough on its own: the operand and the result have to occupy
// the same slot, which the allocator is the only one that can say. What is stated here is the half
// that depends on the instruction - that such a form exists, and that the slot is the width the
// operation works at.
//
// The two are mutually exclusive at one instruction. Both want the r/m field, and there is one.
I32 memoryDefOperand(LowerBase base, LowerInst* inst);

/*
 * Register allocation output.
 *
 * `LowerValue` intentionally has no `.reg` field - the allocator's result is a whole-function
 * mapping that the encoder consumes positionally, and threading it through the IR would put
 * target-specific state on a target-independent structure. Instead, `allocateRegisters` produces
 * one `InstRegs` record per instruction (see below), which the encoder in gen.cpp consumes in
 * lockstep with its own instruction walk.
 */

// One step of a register permutation. `swap` marks the entry as an exchange rather than a copy:
// sequencing a parallel copy whose sources and destinations overlap cyclically needs one, and
// x86's `xchg` provides it without having to find a free scratch register.
struct RegMove {
    RegId from;
    RegId to;
    bool swap = false;
};

// Resolved physical registers (or stack slots, for RegClass::StackReg) for a single instruction.
// `uses`/`creates` are parallel to that instruction's `used()`/`created()` buffers, in the same
// order, and name where the encoder will find (or put) each operand *at this instruction* - which
// is not necessarily where the value lives the rest of the time.
struct InstRegs {
    Array<RegId> uses;
    Array<RegId> creates;

    // Moves emitted immediately before the instruction: they bring operands from their home
    // registers into the places this instruction requires (fixed-register constraints, or the
    // destination of a destructive two-address encoding), and carry values into a successor's phi
    // registers at a terminator. Already sequenced - emit them in order.
    Array<RegMove> moves;

    // Moves emitted immediately after the instruction, carrying a result out of the fixed register
    // the encoding had to write it to and into its home.
    Array<RegMove> postMoves;
};

// Register assignments for every instruction in one block, in the order:
// block->instructions (in order), followed by exactly one entry for block->terminator.
struct BlockRegs {
    Array<InstRegs> insts;
};

/*
 * Frame objects.
 *
 * Anything the function keeps on the stack is an *abstract* slot while the allocator is running:
 * the allocator decides that a value needs stack space, and frame layout decides afterwards where
 * that space is, because the answer depends on things the allocator does not know yet - how many
 * registers ended up needing saving, whether a frame pointer is required, how much alignment the
 * calls in the function demand.
 *
 * A slot is named by a `RegId` of class `StackReg` whose index is its `StackSlotId`, so a value
 * living on the stack and a value living in a register are the same kind of thing everywhere the
 * allocator handles locations.
 */

using StackSlotId = U16;
static constexpr StackSlotId kInvalidSlot = 0xffff;

// Spill slots are grouped by width so that a slot is reused only by values that fit it exactly,
// which keeps first-fit reuse and alignment simple and stops a 4-byte value from pinning down
// 64 bytes of frame. Sizes are 4, 8, 16, 32 and 64 bytes.
enum class StackSlotClass: U8 {
    Slot32, Slot64, Slot128, Slot256, Slot512,
};

static constexpr Size kStackSlotClassCount = 5;

inline U32 stackSlotSize(StackSlotClass c) { return 4u << (Size)c; }

// The slot class a value of this type needs. Every scalar the lowering produces is at most eight
// bytes wide; the wider classes exist for the vector values the register model already describes and
// the encoders do not produce yet.
//
// Asked by the allocator when it spills a value and by the memory-operand table when it decides
// whether an instruction may read one in place, which have to agree: a slot is exactly as wide as
// the value in it, and an access of any other width would take a neighbouring value with it.
inline StackSlotClass stackSlotClassFor(LowerType type) {
    return type == LowerType::Int32 || type == LowerType::Float32
        ? StackSlotClass::Slot32
        : StackSlotClass::Slot64;
}

// What a slot is for. Frame layout puts each kind in its own region, because they answer to
// different rules: incoming arguments live in the *caller's* frame above the return address and
// cannot be moved, locals have to keep their addresses for as long as the function runs, and spill
// slots are the only ones that may be shared between values whose lives do not overlap.
enum class StackSlotKind: U8 {
    Spill,
    Local,       // a fixed-size alloca
    IncomingArg, // an argument the caller left on the stack
};

struct StackSlot {
    StackSlotKind kind = StackSlotKind::Spill;
    StackSlotClass slotClass = StackSlotClass::Slot64;
    U32 size = 8;
    U32 alignment = 8;

    // For an IncomingArg, its byte offset within the argument area, which is what fixes its
    // address: the caller wrote it there and the convention decided where there is. Unused for the
    // kinds this frame places itself.
    U32 argOffset = 0;
};

// A reference to a frame object from an instruction, before frame layout has run. The addend allows
// a reference into the middle of a slot - an element of a local array, the second half of a value
// spilled as two words.
struct FrameReference {
    StackSlotId slot = kInvalidSlot;
    I32 addend = 0;
};

// Everything the function puts on the stack, collected while registers are being allocated and
// consumed by frame layout. Nothing here has an address yet.
struct FrameObjects {
    Array<StackSlot> slots;

    // Frame objects individual instructions refer to: an alloca's local, and later a spilled
    // operand's slot. Keyed by instruction because the reference belongs to the instruction rather
    // than to any value it produces.
    HashMap<LowerInst*, FrameReference> references;

    // Set by an alloca whose size is not known until the function runs. Such a function has to move
    // rsp at runtime, so every fixed frame object needs an address that survives that - which means
    // a frame pointer, whatever the frame-pointer mode asks for.
    bool hasDynamicAlloca = false;

    // Bytes of outgoing argument area: enough for the call in this function that passes the most on
    // the stack, and zero for a function whose calls all fit in registers.
    //
    // The area is reserved once by the prologue rather than opened and closed around each call, and
    // it is always the lowest part of the frame - a callee looks for its stack arguments at the
    // stack pointer, so nothing may sit between them and it. Reserving it once is what keeps rsp
    // still for the whole body, which in turn is what lets a frame be addressed through rsp at all.
    U32 argAreaSize = 0;

    // The largest alignment any call in this function requires of rsp. The frame is padded so that
    // the prologue leaves rsp on that boundary.
    U32 callAlignment = 8;

    StackSlotId add(StackSlot slot) {
        slots.push(slot);
        return StackSlotId(slots.size() - 1);
    }

    bool isEmpty() const { return slots.isEmpty(); }
};

/*
 * Rematerialization.
 *
 * A value cheap enough to recompute does not need to be kept anywhere. Instead of a register or a
 * frame slot, its web is given a *recipe*: the one instruction that recreates it, which is emitted
 * afresh into a scratch register at each instruction that reads it. The definition itself then emits
 * nothing at all, and the value occupies no location between its uses - which is the point, since
 * the values this applies to are exactly the ones whose live ranges are long and whose contents
 * never change.
 *
 * A recipe has to be reproducible at every point the value is live: side-effect free, independent of
 * anything the program can write, and legal wherever it lands. All four kinds below are constants in
 * that sense - an immediate, the address of a global or a function, and the address of a fixed frame
 * object, which is a constant offset from a base register the frame keeps valid for the whole
 * function.
 *
 * A recipe is named by a RegId of class RematReg whose index is its position in
 * FunctionRegs::remats, so a rematerializable value, a value in a slot and a value in a register are
 * the same kind of thing everywhere a location is handled.
 */
struct Remat {
    enum Kind: U8 {
        Immediate,       // mov r, imm
        GlobalAddress,   // lea r, [rip + global]
        FunctionAddress, // lea r, [rip + function]
        FrameAddress,    // lea r, [base + slot]
    };

    Kind kind = Immediate;
    LowerType type = LowerType::Int64;

    U64 imm = 0;                        // Immediate
    LowerGlobal* global = nullptr;      // GlobalAddress
    LowerFunction* function = nullptr;  // FunctionAddress
    FrameReference frame;               // FrameAddress
};

// Where each value lives between the instructions that touch it, indexed by the dense LiveId that
// buildLiveness assigns. This is the allocation proper; the per-instruction InstRegs above are what
// the encoder needs to emit it, and say where an operand sits *at one instruction*, which is not
// always the same place.
//
// Keeping both on FunctionRegs is what lets the verifier check one against the other without
// knowing anything about how the allocator arrived at either.
struct Allocation {
    // One location per LiveId, kInvalidReg for a value that never needed one.
    Array<RegId> locations;

    // The location holding `id` at instruction index `point`. Every value has a single location for
    // the whole of its life today, so `point` is ignored - it is in the signature because that is
    // the question callers should be asking, and the one that gets a different answer once a
    // value's life is split into segments with a location each.
    RegId locationOf(LiveId id, U32 point) const {
        return id < locations.size() ? locations[id] : kInvalidReg;
    }
};

// Register assignments for every block in a function, produced by allocateRegisters()
// and consumed by genFunction().
struct FunctionRegs {
    HashMap<LowerBlock*, BlockRegs> blocks;

    // Where every value lives - see Allocation.
    Allocation allocation;

    // Everything the function needs stack space for - see FrameObjects.
    FrameObjects frame;

    // The recipes for the webs that live nowhere - see Remat. A location of class RematReg indexes
    // this, and every one of them is referenced by exactly one web.
    Array<Remat> remats;

    // Callee-saved registers this function writes, and therefore has to save on entry and restore
    // before every return. Empty for a function that stayed inside its convention's clobber set,
    // which is the common case for a leaf function.
    RegSet usedCalleeSaved;

    // Whether this function establishes rbp as a frame pointer, decided from the IR before
    // allocation ran (functionNeedsFramePointer) and carried here so that frame layout uses the
    // same answer the allocator did. False means rbp was allocatable and may hold a value; the two
    // must never disagree, since the frame is addressed through rbp exactly when this is set.
    bool framePointer = false;
};

/*
 * Frame layout.
 *
 * Runs after allocation, once everything the frame has to hold is known, and turns the abstract
 * slots above into concrete displacements from a base register. This is the only place that knows
 * what the stack looks like; the encoders ask it for an address and emit one.
 *
 * With a frame pointer the layout is
 *
 *     [rbp + 16 + n]   incoming stack argument at offset n
 *     [rbp + 8]        return address
 *     [rbp]            caller's rbp
 *     [rbp - 8k]       saved callee-saved registers
 *     [rbp - ...]      locals and spill slots
 *     [rsp + n]        outgoing argument area                <- rsp after the prologue
 *
 * and without one the same objects hang off rsp instead, which works because rsp then stays put for
 * the whole body - the argument area is reserved once by the prologue rather than opened around
 * each call, and a function that moves rsp any other way (a dynamic alloca) has a frame pointer.
 *
 * The outgoing area is the one thing always addressed through rsp rather than through `base`: a
 * callee finds its stack arguments at the stack pointer, so the area has to stay at the bottom even
 * in a function whose rsp moves. A dynamic alloca therefore re-establishes it below the memory it
 * allocated (see genAlloca).
 */
struct FrameLayout {
    // Callee-saved registers the prologue pushes, in ascending register order.
    RegSet savedRegs;

    // Set when rbp is established as the base for fixed frame objects. Costs a push, a move and a
    // register; see FramePointerMode for when it is worth it.
    bool framePointer = false;

    // The register frame references are relative to: rbp when there is a frame pointer, rsp when
    // there is not.
    RegId base = kInvalidReg;

    // Bytes the prologue subtracts from rsp: the outgoing argument area, the locals and spill
    // slots, and any padding needed to leave rsp on the boundary the calls in this function require.
    U32 fixedSize = 0;

    // Bytes of that reserved for outgoing arguments, at the very bottom. An outgoing argument at
    // convention offset n is at [rsp + n], and a dynamic allocation has to leave this much below
    // itself so that the next call still finds it there.
    U32 argAreaSize = 0;

    // The boundary a dynamic allocation has to round its size up to, so that moving rsp at run time
    // preserves the alignment the prologue established.
    U32 dynamicAlignment = 8;

    // Displacement from `base` for each slot, indexed by StackSlotId.
    Array<I32> slotOffset;

    // Whether the function needs any prologue at all.
    bool isEmpty() const { return savedRegs.isEmpty() && !framePointer && fixedSize == 0; }

    I32 offsetOf(FrameReference ref) const {
        assertTrue(ref.slot < slotOffset.size());
        return slotOffset[ref.slot] + ref.addend;
    }
};

FrameLayout computeFrameLayout(Context& ctx, LowerBase base, LowerFunction& fun, const Constraints& constraints, const FunctionRegs& regs);

// Whether this function establishes a frame pointer, which decides whether rbp is a register the
// allocator may hand out. Answered from the IR and the settings alone, so that it can be asked
// before allocation starts and its answer given to both the allocator and frame layout - see the
// comment at the top of frame.cpp.
bool functionNeedsFramePointer(Context& ctx, LowerBase base, LowerFunction& fun);

// Checks that the offsets a layout produced describe a frame its objects fit in, and that no two of
// them land on the same bytes. Both failures corrupt memory rather than producing a visibly wrong
// register, so neither shows up in a golden; genFunction runs this in debug builds.
bool verifyFrameLayout(Context& ctx, LowerFunction& fun, const FrameObjects& objects, const FrameLayout& layout);

struct AsmBlock {
    LowerBlock* block;
    U32 startOffset;
    U32 endOffset;
};

// A reference to a not-yet-known code offset (a block start or a function entry point) that
// needs to be patched into the instruction stream once all code has been emitted.
// Used for jump/call targets and RIP-relative global/function address loads.
struct AsmRelocation {
    // Offset in the buffer of the 4-byte rel32 field to patch.
    // The patched value is `symbolOffset - (siteOffset + 4)`, i.e. relative to the byte
    // immediately following the field (matching how the CPU computes RIP-relative offsets).
    U32 siteOffset;

    // Resolution target: exactly one of these is set.
    // `function` is used for calls/address-loads that target a (possibly not-yet-emitted)
    // function elsewhere in the module; `block` is used for intra-function jumps; `global` is
    // used for address-loads of module-level data (see AsmModule::addGlobal).
    LowerFunction* function = nullptr;
    LowerBlock* block = nullptr;
    LowerGlobal* global = nullptr;
};

struct AsmModule {
    explicit AsmModule(Size initialSize = 4096): buffer(initialSize) {}

    Net::BufferWriter buffer;
    Array<AsmBlock> blocks;
    Array<AsmRelocation> relocations;
    HashMap<LowerBlock*, U32> blockOffsets;
    HashMap<LowerFunction*, U32> functionOffsets;
    HashMap<LowerGlobal*, U32> globalOffsets;

    void startBlock(LowerBlock* block) {
        auto b = blocks.push(AsmBlock {
            .block = block,
            .startOffset = U32(buffer.offset()),
            .endOffset = 0,
        });

        blockOffsets.add(block, U32(b - blocks.begin()));
    }

    void endBlock(LowerBlock* block) {
        auto b = blockOffsets.getValue(block);
        assertTrue(b.isJust());

        blocks[b.unwrap()].endOffset = U32(buffer.offset());
    }

    void startFunction(LowerFunction* fun) {
        functionOffsets.add(fun, U32(buffer.offset()));
    }

    // Appends a global's data to the buffer and records the offset its address-loads resolve to.
    // Globals are emitted into the same flat buffer as code (this is not an object-file writer -
    // there are no sections), so callers should emit all functions first and all globals after,
    // keeping executable and non-executable bytes from interleaving. Offsets are 16-byte aligned
    // so that a global is never split across a cache line by whatever preceded it.
    void addGlobal(LowerGlobal* global) {
        while(buffer.offset() & 15) buffer.writeByte(0);

        globalOffsets.add(global, U32(buffer.offset()));
        buffer.writeBytes(global->initialContents.data(), global->initialContents.size());
    }

    // Records a placeholder relocation at the rel32 field about to be written at the buffer's
    // current offset, then writes a placeholder 0 in its place. Call resolveRelocations() once
    // all functions referenced by any relocation have been emitted.
    void addRelocation(LowerBlock* target) {
        relocations.push(AsmRelocation { .siteOffset = U32(buffer.offset()), .block = target });
        buffer.writeInt<LittleEndian>(0);
    }

    void addRelocation(LowerFunction* target) {
        relocations.push(AsmRelocation { .siteOffset = U32(buffer.offset()), .function = target });
        buffer.writeInt<LittleEndian>(0);
    }

    void addRelocation(LowerGlobal* target) {
        relocations.push(AsmRelocation { .siteOffset = U32(buffer.offset()), .global = target });
        buffer.writeInt<LittleEndian>(0);
    }

    // Patches every recorded relocation with the now-known offset of its target.
    // Must be called after every block/function referenced by any relocation has been emitted.
    void resolveRelocations();
};

// Represents an address calculation (base + index * scale) + displacement.
// Used with two different instruction kinds:
//  - X86Address: purely embedded into whatever instruction uses it (Load/Store) - never
//    materialized into a register of its own, so its result is always Implicit.
//  - X86Lea: materializes the computed address into a real register (LEA), e.g. for pointer
//    arithmetic that doesn't immediately feed a Load/Store.
struct LowerInstX86Address: LowerInstSingle {
    LowerInstX86Address(LowerInst::Kind kind, U32 name, LowerPtr<LowerValue> base, LowerPtr<LowerValue> index, U8 scale, U32 displacement):
        LowerInstSingle(kind, name, LowerType::Pointer),
        base(base), index(index), displacement(displacement), scale(scale)
    {
        assertTrue(kind == LowerInst::X86Address || kind == LowerInst::X86Lea);

        if(base) {
            usedCount = index ? 2 : 1;
        } else if(index) {
            usedCount = 1;
        }

        if(kind == LowerInst::X86Address) {
            result.flags |= LowerValue::Implicit;
        }
    }

    LowerPtr<LowerValue> base, index;
    U32 displacement;
    U8 scale;
};

void transformFunction(LowerBase base, LowerFunction& fun);
FunctionRegs allocateRegisters(Context& ctx, LowerBase base, LowerFunction& fun);

// Checks that an allocation actually delivers every value to every instruction that reads it, by
// simulating the register and stack contents the emitted code will produce and comparing them
// against what each instruction expects to find. Returns false and logs the first disagreement per
// function; allocateRegisters runs it on its own result in debug builds.
//
// It knows nothing about how the allocation was arrived at - only about FunctionRegs, the liveness
// sets and the constraint tables - so it stays a valid check as the allocator gains live intervals
// with holes, phi webs, stack homes and split locations.
bool verifyAllocation(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live, const Constraints& constraints, const FunctionRegs& regs);

// Called (if non-null) once for every instruction/terminator emitted, with the byte range it
// occupies in `to.buffer` - used by test harnesses to build an annotated disassembly listing
// without genFunction itself needing to know anything about how that listing is formatted.
//
// `inst` is null for the function prologue, which belongs to the function rather than to any one
// instruction; `regs` is empty in that case.
using InstEmitCallback = void (*)(void* ctx, LowerInst* inst, const InstRegs& regs, U32 startOffset, U32 endOffset);

void genFunction(Context& context, LowerBase base, AsmModule& to, LowerFunction& fun, FunctionRegs& regs, InstEmitCallback onInst = nullptr, void* onInstCtx = nullptr);
