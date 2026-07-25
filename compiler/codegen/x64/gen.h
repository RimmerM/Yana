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

// Byte count above which a Copy/SetPattern with a compile-time size stops being straight-lined into
// plain moves and takes the rep-prefixed string instruction instead. Chosen once, in
// transformFunction (see selectBlockOpEncoding), and recorded on the instruction.
static constexpr U64 kMaxUnrolledMemOp = 32;

struct Constraints {
    Constraints();
    const InstConstraints* getConstraints(LowerBase base, LowerInst* inst) const;
    const InstConstraints& getCall(LowerCallType type) const;

private:
    // Copy/SetPattern have two encodings with very different register requirements: `rep movsb`/
    // `rep stosb` (movsb/stosb) demand fixed registers and consume them, while the unrolled-mov
    // form (movsbImm/stosbImm) works out of whatever registers the operands already occupy. Which
    // one applies is decided once by transformFunction and read back off the instruction, so this
    // table and genCopy/genSetPattern cannot disagree about it.
    InstConstraints mul, div, rem, shift, movsb, stosb, movsbImm, stosbImm;
    InstConstraints call[(Size)LowerCallType::LastType + 1];
};

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

// Register assignments for every block in a function, produced by allocateRegisters()
// and consumed by genFunction().
struct FunctionRegs {
    HashMap<LowerBlock*, BlockRegs> blocks;
};

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

// Called (if non-null) once for every instruction/terminator emitted, with the byte range it
// occupies in `to.buffer` - used by test harnesses to build an annotated disassembly listing
// without genFunction itself needing to know anything about how that listing is formatted.
using InstEmitCallback = void (*)(void* ctx, LowerInst* inst, const InstRegs& regs, U32 startOffset, U32 endOffset);

void genFunction(Context& context, LowerBase base, AsmModule& to, LowerFunction& fun, FunctionRegs& regs, InstEmitCallback onInst = nullptr, void* onInstCtx = nullptr);
