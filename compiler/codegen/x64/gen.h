#pragma once

#include "Net/Buffer.h"
#include "Net/Stream.h"
#include "../../lower/lower_inst.h"

static constexpr Size kMaxRegInputs = 16;

enum RegClass {
    GenReg,
    XmmReg,
    StackReg,
};

using RegId = I16;

static constexpr Size kRegClassCount = 3;
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

struct ClassConstraints {
    // Maps instruction argument index -> register id.
    RegId args[kMaxRegInputs];
    RegId results[kMaxRegInputs];
};

// Given an instruction, contains the register id (if any) required for each argument and result.
// At most kMaxReg constraints are returned, as no more registers are available.
// Instructions that generate additional results are presumed to handle them through the stack.
// This only concerns special instructions that require a single specific register for some result.
// General register classes are handled by the type system instead
struct InstConstraints {
    InstConstraints();

    // Argument and result constraints.
    ClassConstraints constraints[kRegClassCount];

    // Bitmask of clobbered registers, indexed by register id.
    U64 clobber = 0;
};

// The half of a calling convention that concerns the function being compiled rather than its call
// sites. InstConstraints above describes what a *call* does to registers where it appears; this
// describes what a function compiled with the convention owes the caller it returns to, and is what
// the prologue and epilogue are generated from.
//
// The two halves are the same contract seen from opposite sides - a register a call does not
// clobber is exactly a register the callee has to give back - so constraint.cpp states both and
// checks them against each other rather than deriving either.
struct CallConvention {
    // Callee-saved general registers, restricted to kAllocatableRegs. rsp and rbp are preserved
    // too, but they are reserved rather than handed out, so keeping them valid is the frame code's
    // business rather than the prologue's.
    U64 calleeSaved = 0;

    // What rsp must be a multiple of at the point a call of this convention is executed, before the
    // call pushes its return address. A convention the compiler is on both sides of can leave this
    // at 8; an external one generally cannot, because its callees are entitled to assume the
    // alignment when they spill a vector register.
    U32 stackAlignment = 8;
};

// Byte count above which a Copy/SetPattern with a compile-time size stops being straight-lined into
// plain moves and takes the rep-prefixed string instruction instead. Chosen once, in
// transformFunction (see selectBlockOpEncoding), and recorded on the instruction.
static constexpr U64 kMaxUnrolledMemOp = 32;

struct Constraints {
    Constraints();
    const InstConstraints* getConstraints(LowerBase base, LowerInst* inst) const;
    const InstConstraints& getCall(LowerCallType type) const;
    const CallConvention& getConvention(LowerCallType type) const;

private:
    // Copy/SetPattern have two encodings with very different register requirements: `rep movsb`/
    // `rep stosb` (movsb/stosb) demand fixed registers and consume them, while the unrolled-mov
    // form (movsbImm/stosbImm) works out of whatever registers the operands already occupy. Which
    // one applies is decided once by transformFunction and read back off the instruction, so this
    // table and genCopy/genSetPattern cannot disagree about it.
    InstConstraints mul, div, rem, shift, movsb, stosb, movsbImm, stosbImm;
    InstConstraints call[(Size)LowerCallType::LastType + 1];
    CallConvention convention[(Size)LowerCallType::LastType + 1];
};

/*
 * Instruction constraint queries.
 *
 * The tables above are indexed per register class and skip operands that don't occupy a register at
 * all, so the mapping from "operand N of this instruction" to "entry N of the table" is not the
 * identity. Resolving it in one place, from the instruction alone, keeps every caller from having
 * to thread a running per-class counter through its own loop - and keeps the allocator and the
 * verifier that checks it reading the same tables the same way.
 */

inline RegClass classForType(LowerType type) {
    if(isIntLike(type)) {
        return GenReg;
    } else {
        return XmmReg;
    }
}

struct InstShape {
    const InstConstraints* c = nullptr;

    // First used() index that is a constrained argument. Call's used()[0] is the callee, not an
    // argument - except for a syscall, which has no callee at all: its used()[0] is the syscall
    // number, and is the first constrained operand.
    Size argStart = 0;

    // Set for a return, whose values are constrained like a call's *results* rather than its
    // arguments. A return's clobber set is deliberately ignored throughout: nothing is live once
    // the function has returned, so there is nothing left for it to protect.
    bool isReturn = false;
};

InstShape shapeOf(LowerBase base, const Constraints& constraints, LowerFunction& fun, LowerInst* inst);

// The fixed register operand `i` has to be in when the instruction executes, if any.
RegId wantForUse(LowerBase base, LowerInst* inst, const InstShape& shape, Size i);

// The fixed register result `i` is produced in, if any.
RegId wantForResult(LowerInst* inst, const InstShape& shape, Size i);

// Every general register this instruction writes behind the operands' backs: the ones it clobbers,
// plus the ones the parallel copy in front of it writes to satisfy fixed-register constraints. A
// value that has to survive the instruction, and an operand that isn't itself placed by that
// parallel copy, both have to stay out of these.
U64 writtenRegisters(LowerBase base, LowerInst* inst, const InstShape& shape);

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

    // Position among the slots of the same kind, in the order they were created. For an incoming
    // argument this is what fixes its address: argument n sits 8n bytes above the return address.
    U32 ordinal = 0;
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

    // Set if any call passes arguments by pushing them. rsp then moves during the argument setup,
    // so an rsp-relative reference to a fixed frame object taken in the middle of it would be wrong
    // - which is the second reason a frame pointer can be forced.
    bool hasPushedCallArgs = false;

    // The largest alignment any call in this function requires of rsp. The frame is padded so that
    // the prologue leaves rsp on that boundary.
    U32 callAlignment = 8;

    StackSlotId add(StackSlot slot) {
        // Slots of one kind are numbered in creation order, which is what gives an incoming
        // argument its position in the caller's frame.
        for(auto& s: slots) {
            if(s.kind == slot.kind) slot.ordinal++;
        }

        slots.push(slot);
        return StackSlotId(slots.size() - 1);
    }

    bool isEmpty() const { return slots.isEmpty(); }
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

    // Callee-saved registers this function writes, and therefore has to save on entry and restore
    // before every return. Empty for a function that stayed inside its convention's clobber set,
    // which is the common case for a leaf function.
    U64 usedCalleeSaved = 0;
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
 *     [rbp + 16 + 8n]  incoming stack argument n
 *     [rbp + 8]        return address
 *     [rbp]            caller's rbp
 *     [rbp - 8k]       saved callee-saved registers
 *     [rbp - ...]      locals and spill slots        <- rsp after the prologue
 *
 * and without one the same objects hang off rsp instead, which only works because rsp then stays
 * put for the whole body: pushed call arguments are popped again immediately, and a function that
 * moves rsp any other way (a dynamic alloca) is required to have a frame pointer.
 */
struct FrameLayout {
    // Callee-saved registers the prologue pushes, in ascending register order.
    U64 savedRegs = 0;

    // Set when rbp is established as the base for fixed frame objects. Costs a push, a move and a
    // register; see FramePointerMode for when it is worth it.
    bool framePointer = false;

    // The register frame references are relative to: rbp when there is a frame pointer, rsp when
    // there is not.
    RegId base = kInvalidReg;

    // Bytes the prologue subtracts from rsp for locals and spill slots, including any padding
    // needed to leave rsp on the boundary the calls in this function require.
    U32 fixedSize = 0;

    // The boundary a dynamic allocation has to round its size up to, so that moving rsp at run time
    // preserves the alignment the prologue established.
    U32 dynamicAlignment = 8;

    // Displacement from `base` for each slot, indexed by StackSlotId.
    Array<I32> slotOffset;

    // Whether the function needs any prologue at all.
    bool isEmpty() const { return savedRegs == 0 && !framePointer && fixedSize == 0; }

    I32 offsetOf(FrameReference ref) const {
        assertTrue(ref.slot < slotOffset.size());
        return slotOffset[ref.slot] + ref.addend;
    }
};

FrameLayout computeFrameLayout(Context& ctx, LowerBase base, LowerFunction& fun, const Constraints& constraints, const FunctionRegs& regs);

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
