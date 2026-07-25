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
    return RegClass(id >> 4);
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

struct Constraints {
    Constraints();
    const InstConstraints* getConstraints(LowerBase base, LowerInst* inst) const;
    const InstConstraints& getCall(LowerCallType type) const;

private:
    InstConstraints mul, div, rem, shift, movsb, stosb;
    InstConstraints call[(Size)LowerCallType::LastType + 1];
};

struct AsmBlock {
    LowerBlock* block;
    U32 startOffset;
    U32 endOffset;
};

struct AsmRelocation {
    union {
        LowerFunction* function;
        LowerBlock* block;
    };
    void (*write)(Net::Writer& writer, U32 ownOffset, U32 symbolOffset);
};

struct AsmModule {
    Net::BufferWriter buffer;
    Array<AsmBlock> blocks;
    Array<AsmRelocation> relocations;
    HashMap<LowerBlock*, U32> blockOffsets;

    void startBlock(LowerBlock* block) {
        auto b = blocks.push(AsmBlock {
            .block = block,
            .startOffset = U32(buffer.offset()),
            .endOffset = 0,
        });

        blockOffsets.add(block, b - blocks.begin());
    }

    void endBlock(LowerBlock* block) {
        auto b = blockOffsets.getValue(block);
        assertTrue(b.isJust());

        blocks[b.unwrap()].endOffset = buffer.offset();
    }
};

// Represents an address calculation (base + index * scale) + displacement.
struct LowerInstX86Address: LowerInstSingle {
    LowerInstX86Address(U32 name, LowerPtr<LowerValue> base, LowerPtr<LowerValue> index, U8 scale, U32 displacement):
        LowerInstSingle(X86Address, name, LowerType::Pointer),
        base(base), index(index), displacement(displacement), scale(scale)
    {
        if(base) {
            usedCount = index ? 2 : 1;
        } else if(index) {
            usedCount = 1;
        }
    }

    LowerPtr<LowerValue> base, index;
    U32 displacement;
    U8 scale;
};

void transformFunction(LowerBase base, LowerFunction& fun);
void genFunction(Context& ctx, AsmModule& to, LowerFunction& fun);
