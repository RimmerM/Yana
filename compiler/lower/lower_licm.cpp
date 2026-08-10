#include "lower_licm.h"
#include "lower_builder.h"

namespace {

// How far an address is followed back through constant offsets. Field addressing is one `add` and
// the occasional pair of them, so a budget rather than a loop guard is enough - and it keeps this
// question at a fixed cost per candidate.
static const U32 kOffsetDepth = 4;

// An address as a base value and a constant displacement from it. `add %p, 8` is the only shape the
// lowering builds for a field, so peeling it is the whole of the normalization: everything else -
// an index, a scaled offset, a pointer that came out of memory - is its own base at offset zero.
struct Address {
    LowerValue* value;
    U64 offset;
};

Address addressOf(LowerBase base, LowerValue* value) {
    Address result { value, 0 };

    for(U32 i = 0; i < kOffsetDepth; i++) {
        auto inst = result.value->inst();
        if(inst->kind != LowerInst::Add) break;

        auto binary = (LowerInstBinary*)inst;
        auto rhs = base[binary->rhs];
        if(rhs->inst()->kind != LowerInst::Imm || !isInt(rhs->type)) break;

        result.offset += ((LowerImm*)rhs->inst())->i;
        result.value = base[binary->lhs];
    }

    return result;
}

// Whether an instruction may write storage a hoisted read could see. Asked of the whole loop rather
// than of a pair of addresses - see lower_licm.h for why the crude answer is the one taken here.
bool writesStorage(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Store:
        case LowerInst::Copy:
        case LowerInst::SetPattern:
        case LowerInst::Call:
        case LowerInst::Intrinsic:
            return true;
        default:
            return false;
    }
}

// How far an instruction reaches past its own base address, for the two kinds that touch memory.
// Zero for everything else, which is what makes the search below a filter rather than a switch.
U64 accessExtent(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Load:  return ((LowerInstLoad*)inst)->getWidth();
        case LowerInst::Store: return ((LowerInstStore*)inst)->getWidth();
        default:               return 0;
    }
}

LowerValue* accessAddress(LowerBase base, LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Load:  return base[((LowerInstLoad*)inst)->from];
        case LowerInst::Store: return base[((LowerInstStore*)inst)->to];
        default:               return nullptr;
    }
}

/*
 * Whether the bytes this load reads are storage that exists whether or not the loop body runs.
 *
 * Two answers, both about the address rather than about the loop - see lower_licm.h. The second is
 * stated as "the object extends at least this far from its base", which is what a dominating access
 * at a further offset says: an access at `%p + 12` of four bytes is a statement that `%p` names
 * sixteen bytes, and `%p + 0` of eight is inside them.
 */
bool safeToSpeculate(LowerBase base, LowerFunction& fun, const DominatorTree& dominators,
                     LowerBlock* preheader, const Address& address, U64 extent) {
    auto inst = address.value->inst();

    if(inst->kind == LowerInst::Alloca) {
        auto size = base[((LowerInstAlloca*)inst)->byteCount];
        if(size->inst()->kind != LowerInst::Imm) return false;

        return address.offset + extent <= ((LowerImm*)size->inst())->i;
    }

    // The accesses that reach this base, at whatever constant offset of their own, from a block the
    // preheader is reached through. A record read at one field vouches for every field below it.
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(block != preheader && !block->dominates(preheader, dominators)) continue;

        for(auto instPtr: block->instructions.contents(base)) {
            auto user = base[instPtr];

            auto width = accessExtent(user);
            if(!width) continue;

            auto at = accessAddress(base, user);
            if(!at) continue;

            auto other = addressOf(base, at);
            if(other.value != address.value) continue;

            if(other.offset + width >= address.offset + extent) return true;
        }
    }

    return false;
}

// The one predecessor of a loop header from outside it, where that block exists and does nothing but
// enter the loop. An instruction put there runs exactly when the loop is entered, which is half of
// what makes moving a read into it sound - the other half is `safeToSpeculate`.
LowerBlock* preheaderOf(LowerBase base, const LoopInfo& loops, LowerBlock* header) {
    if(header->incoming.size() != 2) return nullptr;

    auto first = base[header->incoming.get(base, 0)];
    auto second = base[header->incoming.get(base, 1)];

    auto firstInside = loops.contains(header->index, first->index);
    if(firstInside == loops.contains(header->index, second->index)) return nullptr;

    auto pre = firstInside ? second : first;
    if(!pre->terminator || base[pre->terminator]->kind != LowerInst::Jmp) return nullptr;

    return pre;
}

} // namespace

void hoistLoopLoads(LowerBase base, LowerModule& module, LowerFunction& fun,
                    const LoopAnalysis& analysis)
{
    if(fun.blocks.size() < 2) return;

    // The caller's, and valid for the whole walk: everything below moves instructions between
    // blocks that already exist, so neither the loop structure nor the dominance relation moves
    // with them. See LoopAnalysis.
    auto& loops = analysis.loops;
    auto& dominators = analysis.dominators;

    /*
     * Innermost first, and repeated until nothing moves. A load leaving an inner loop lands in that
     * loop's preheader, which is a block the enclosing loop contains - so carrying it the rest of
     * the way out is another round rather than another rule, and the walk below is in no particular
     * order to begin with.
     */
    auto changed = true;
    while(changed) {
        changed = false;

        for(auto headerPtr: fun.blocks.contents(base)) {
            auto header = base[headerPtr];
            if(!loops.isHeader(header->index)) continue;

            auto preheader = preheaderOf(base, loops, header);
            if(!preheader) continue;

            // What the loop does to storage, once per loop rather than once per candidate.
            auto writes = false;
            for(auto blockPtr: fun.blocks.contents(base)) {
                auto block = base[blockPtr];
                if(!loops.contains(header->index, block->index)) continue;

                for(auto instPtr: block->instructions.contents(base)) {
                    if(writesStorage(base[instPtr])) { writes = true; break; }
                }

                if(writes) break;
            }

            if(writes) continue;

            for(auto blockPtr: fun.blocks.contents(base)) {
                auto block = base[blockPtr];
                if(!loops.contains(header->index, block->index)) continue;

                // Inline: one of these per block, holding the instructions that stay while the list
                // it came from is rebuilt - the same shape lower_fold.cpp and lower_cse.cpp use.
                SmallArray<LowerPtr<LowerInst>, 32> kept;
                auto moved = false;

                for(auto instPtr: block->instructions.contents(base)) {
                    auto inst = base[instPtr];

                    if(inst->kind != LowerInst::Load) {
                        kept.push(instPtr);
                        continue;
                    }

                    auto load = (LowerInstLoad*)inst;
                    auto from = base[load->from];

                    // The address has to be the same one every iteration, which for a value in SSA
                    // form is simply where it was defined. An argument belongs to no block and is
                    // outside every loop there is.
                    auto definition = from->inst()->block;
                    if(definition && loops.contains(header->index, base[definition]->index)) {
                        kept.push(instPtr);
                        continue;
                    }

                    auto address = addressOf(base, from);
                    if(!safeToSpeculate(base, fun, dominators, preheader, address, load->getWidth())) {
                        kept.push(instPtr);
                        continue;
                    }

                    /*
                     * Appended to the preheader, which puts it in front of that block's terminator:
                     * a terminator is not in the instruction list. The instruction keeps its
                     * operands and its result, so every reader of it stays pointed at the same
                     * value - and the preheader dominates the whole loop, so every one of them is
                     * still dominated by the definition.
                     *
                     * Through `detach` first, because `addInst` is what *records* an instruction's
                     * uses: adding one that is already in its operands' use lists would put it there
                     * twice, and the validator counts both directions.
                     */
                    detach(base, inst);
                    inst->block = nullptr;
                    preheader->addInst(base, inst);
                    moved = true;
                }

                if(!moved) continue;

                block->instructions.clear();
                for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
                changed = true;
            }
        }
    }
}
