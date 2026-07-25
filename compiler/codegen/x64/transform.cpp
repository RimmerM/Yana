#include "gen.h"
#include "x64_util.h"

// Checks if the flags register can be modified by running this instruction.
inline bool modifiesFlags(LowerInst* inst) {
    return isBinary(inst) || isCall(inst) || isUnaryArith(inst) || inst->kind == LowerInst::Alloca;
}

// Checks if this is a binary arithmetic instruction that supports an immediate right-hand operand.
inline bool allowsImmRhs(U32 kind) {
    return
        kind == LowerInst::Add || kind == LowerInst::Sub || kind == LowerInst::IMul ||
        kind == LowerInst::Shl || kind == LowerInst::Shr || kind == LowerInst::Sar ||
        kind == LowerInst::And || kind == LowerInst::Or || kind == LowerInst::Xor ||
        kind == LowerInst::Cmp;
}

// Checks if this immediate value can possibly be embedded into any instruction.
static bool isEmbeddableImm(LowerImm* imm) {
    // Floats can never be embedded.
    if(!isIntLike(imm->result.type)) return false;

    // X86 can embed up to 4-byte immediates into instructions.
    // For 1-byte ones, we always embed.
    // For 4-byte ones, we embed for <= 2 uses; for higher use counts, it's better to save space instead.
    if(encodeImm8(&imm->result)) {
        return true;
    } else if(encodeImm32(&imm->result) && imm->result.uses.size() <= 2) {
        return true;
    }

    return false;
}

// Checks if this specific instruction can embed the provided embeddable immediate operand.
static bool canEmbedImm(LowerBase base, LowerInst* inst, LowerValue* op) {
    // We only check if the instruction type can embed;
    // register types have already been checked here.
    auto kind = inst->kind;
    if(kind == LowerInst::Set) {
        assertTrue(op == base[((LowerInstUnary*)inst)->from]);
        return true;
    } else if(kind == LowerInst::Cast || kind == LowerInst::Bitcast) {
        return isIntLike(((LowerInstUnary*)inst)->result.type);
    } else if(allowsImmRhs(kind)) {
        return op == base[((LowerInstCmp*)inst)->rhs];
    }

    return false;
}

// Tries to embed this immediate into any instructions that use it.
static bool tryEmbedImm(LowerBase base, LowerImm* imm) {
    if(!isEmbeddableImm(imm)) return false;

    for(auto use: imm->result.uses.contents(base)) {
        if(!canEmbedImm(base, base[use], &imm->result)) return false;
    }

    imm->result.flags |= LowerValue::Implicit;
    return true;
}

// Tries to swap operands to the provided instruction in order to make it easier to perform further optimizations.
// This needs to be done before register allocation,
// since swapping and then embedding may reduce the number of registers needed.
static bool trySwapOperands(LowerBase base, LowerInst* inst) {
    if(!isBinary(inst)) return false;

    auto binary = (LowerInstBinary*)inst;
    if(!isIntLike(binary->result.type)) return false;

    // For register and memory operands, both directions can be encoded, so it is pointless to swap.
    // Because of that, we only check if immediates can swapped.
    if(base[binary->lhs]->inst()->kind != LowerInst::Imm) return false;

    // Only swap for operations that are safe.
    auto kind = binary->kind;
    if(!(kind == LowerInst::Add || kind == LowerInst::Mul || kind == LowerInst::IMul ||
       kind == LowerInst::And || kind == LowerInst::Or || kind == LowerInst::Xor)) return false;

    // Swap lhs with rhs to ensure the immediate is on the right side.
    ::swap(binary->lhs, binary->rhs);
    return true;
}

static bool hasFlagsInterference(LowerBase base, LowerInstCmp* cmp, LowerInst* use, Size startIndex) {
    // Currently, we simply check if there is any interfering instruction below the creation, until we get to the use.
    // TODO: Follow paths between blocks from the definition to the use.
    if(use->block != cmp->block) return true;

    auto block = base[cmp->block];
    auto list = block->instructions.contents(base);

    for(Size i = startIndex + 1; i < list.size(); i++) {
        auto inst = base[list[i]];
        if(inst == use) return false;
        if(modifiesFlags(inst)) return true;
    }

    if(use == base[block->terminator]) return false;
    return true;
}

static bool tryMergeCompare(LowerBase base, LowerInstCmp* cmp, Size index) {
    auto& uses = cmp->result.uses;

    if(uses.size() == 0) {
        cmp->result.flags |= LowerValue::Implicit;
        return true;
    }

    for(auto offset: uses.contents(base)) {
        auto use = base[offset];

        // If the result is used as an actual value, it needs to written to a register.
        if(use->kind != LowerInst::Je && use->kind != LowerInst::Select) return false;

        // Check if there is any instruction between the definition and use that could modify the flags.
        if(hasFlagsInterference(base, cmp, use, index)) return false;
    }

    // The only uses are instructions that can use flags directly, and no interference, so the result can stay as flags.
    cmp->result.flags |= LowerValue::Implicit;

    for(auto offset: uses.contents(base)) {
        auto use = base[offset];

        if(use->kind == LowerInst::Je) {
            ((LowerInstJe*)use)->setEmbeddedCmp(Just(cmp->getCmp()));
        } else if(use->kind == LowerInst::Select) {
            ((LowerInstSelect*)use)->setEmbeddedCmp(Just(cmp->getCmp()));
        }
    }

    return true;
}

void transformFunction(LowerBase base, LowerFunction& fun) {
    auto pass = [&](auto onInst) {
        for(auto b: fun.blocks.contents(base)) {
            Size i = 0;

            for(auto inst: base[b]->instructions.contents(base)) {
                onInst(base[inst], i);
                i++;
            }
        }
    };

    // Move operands into place for later passes.
    pass([&](LowerInst* inst, Size i) {
        trySwapOperands(base, inst);
    });

    pass([&](LowerInst* inst, Size i) {
        // Make immediates implicit where possible.
        if(inst->kind == LowerInst::Imm) {
            tryEmbedImm(base, (LowerImm*)inst);
        }

        // Make comparisons implicit if flags aren't changed between the creation and any uses.
        if(inst->kind == LowerInst::Cmp) {
            tryMergeCompare(base, (LowerInstCmp*)inst, i);
        }
    });
}
