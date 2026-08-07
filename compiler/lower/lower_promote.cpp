#include "lower_promote.h"
#include "lower_inst.h"
#include "lower_builder.h"

/*
 * Promotion, as the textbook algorithm with one deliberate simplification.
 *
 * The standard construction places a phi exactly where a slot's definitions meet - the iterated
 * dominance frontier of the blocks that write it. This places one in *every* block the slot is known
 * to be live into, and then deletes the ones whose alternatives all agree. The two produce the same
 * IR; this one needs no dominance frontier and no dominator-tree walk, and the functions it runs over
 * are small enough that the difference is not measurable.
 *
 * What it does need is to know where a phi would have nothing to say. A slot that has not been
 * written on every path into a block has no value to carry there, so "written on every path" is a
 * plain forward AND-dataflow computed first, and it decides three things at once: where a phi may be
 * placed, that every alternative of one exists, and whether any load reads a slot that was never
 * written - which is the one shape that makes a slot unpromotable rather than merely unpromoted.
 */

namespace {

// How many bits a value of this type occupies in a register, and therefore whether a load narrower
// than the slot's register truncated something on the way out of memory.
U32 registerBits(LowerType type) {
    return type == LowerType::Int32 || type == LowerType::Float32 ? 32 : 64;
}

/*
 * One stack slot this pass decided it can hold in a register.
 *
 * `width` is the memory the slot occupies and `type` the register every access to it agrees on. The
 * two are not the same number: a one-byte scalar record is read into a 64-bit register, and it is
 * that gap the rewrite has to reproduce - see narrowedTo.
 */
struct Slot {
    LowerPtr<LowerValue> address = nullptr;
    U32 width = 0;
    LowerType type = LowerType::Int64;

    // Per block, indexed by LowerBlock::index.
    IndexSet stores;          // whether the block writes the slot at all
    IndexSet available;       // whether every path into the block has written it
    Array<LowerPtr<LowerValue>> entry;   // the phi carrying it in, where one was placed
    Array<LowerPtr<LowerValue>> exit;    // what it holds on the way out, or null
};

/*
 * Whether a value is the address of a slot, and which one - by identity of the alloca's result, so a
 * copy of the address or an offset from it is not one of these and disqualifies the slot below.
 */
Size slotOf(HashMap<U32, U32>& index, LowerPtr<LowerValue> value) {
    auto found = index.getValue(U32(value));
    return found ? Size(found.unwrap()) : maxLimit<Size>;
}

/*
 * The slots worth trying, which are the ones the whole of whose use is a load or a store of the whole
 * thing.
 *
 * Anything else - the address handed to a call, copied, offset into, or stored *as* a value - means
 * the slot is memory something else can see, and no amount of dataflow makes it not so. The width and
 * register type have to be the same at every access for the same reason a phi's alternatives do: what
 * replaces the slot is one register, and one register has one type.
 */
void collectSlots(LowerBase base, LowerFunction& fun, Array<Slot>& into, HashMap<U32, U32>& index) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];
            if(inst->kind != LowerInst::Alloca) continue;

            auto allocation = (LowerInstAlloca*)inst;
            auto address = allocation->created().ptr;

            // A slot of a size no register holds is not one, and neither is one whose size is not a
            // constant here - a variable-sized allocation is memory by construction.
            auto sizeInst = base[allocation->byteCount]->inst();
            if(sizeInst->kind != LowerInst::Imm) continue;

            auto width = U32(((LowerImm*)sizeInst)->i);
            if(width != 1 && width != 2 && width != 4 && width != 8) continue;

            Slot slot;
            slot.address = address - base;
            slot.width = width;

            auto usable = true;
            auto typed = false;

            for(auto userPtr: address->uses.contents(base)) {
                auto user = base[userPtr];
                LowerType type;

                if(user->kind == LowerInst::Load) {
                    auto loadInst = (LowerInstLoad*)user;
                    if(loadInst->getWidth() != width) { usable = false; break; }
                    type = loadInst->created().ptr->type;
                } else if(user->kind == LowerInst::Store) {
                    auto storeInst = (LowerInstStore*)user;

                    // Storing the address *into* the slot is the address escaping into memory, and
                    // is not the same thing as writing the slot.
                    if(storeInst->to != slot.address || storeInst->value == slot.address) {
                        usable = false;
                        break;
                    }

                    if(storeInst->getWidth() != width) { usable = false; break; }
                    type = base[storeInst->value]->type;
                } else if(user->kind == LowerInst::Copy) {
                    /*
                     * A copy of exactly this slot, which is how a value built in a temporary reaches
                     * the local it was built for - and the shape that would otherwise keep every
                     * constructed record in memory.
                     *
                     * It says nothing about the register type, so a slot whose only traffic is copies
                     * is still declined; what it needs is for the count to be this slot's width, so
                     * that what moves is the whole of it and nothing more.
                     */
                    auto copyInst = (LowerInstCopy*)user;
                    auto count = base[copyInst->count]->inst();

                    // One of the two ends, and not the byte count: an address used as a *number* is
                    // an address that outlives the storage, which is the one thing promoting it
                    // cannot survive.
                    if(copyInst->to != slot.address && copyInst->from != slot.address) {
                        usable = false;
                        break;
                    }

                    if(count->kind != LowerInst::Imm || U32(((LowerImm*)count)->i) != width) {
                        usable = false;
                        break;
                    }

                    continue;
                } else {
                    usable = false;
                    break;
                }

                if(!typed) {
                    slot.type = type;
                    typed = true;
                } else if(slot.type != type) {
                    usable = false;
                    break;
                }
            }

            // A slot nothing reads or writes is not worth a phi; leaving it also leaves whatever
            // reserved it, which is a decision for a dead-code pass rather than for this one.
            if(!usable || !typed) continue;

            // A float or a pointer read out of storage narrower than itself is not something this
            // reproduces - and not something anything emits, since both are stored whole.
            if(!isInt(slot.type) && width * 8 != registerBits(slot.type)) continue;

            index.add(U32(slot.address), U32(into.size()));
            into.push(::move(slot));
        }
    }
}

/*
 * Where the slot is known to hold something.
 *
 * A forward must-analysis, `in[b] = AND over predecessors of out[p]` and `out[b] = in[b] || writes`,
 * from the pessimistic end so that a loop whose body writes the slot converges to "written" while one
 * that does not stays unwritten. A block nothing jumps to - the entry, and anything unreachable -
 * starts with nothing, which is what makes an unreachable predecessor deny a phi rather than leave it
 * an alternative that does not exist.
 */
void computeAvailability(LowerBase base, LowerFunction& fun, Slot& slot) {
    auto count = fun.blocks.size();

    slot.available.reset(count);

    IndexSet out;
    out.copyFrom(slot.stores);

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];
            auto incoming = block->incoming.contents(base);

            auto in = incoming.size() != 0;
            for(auto predPtr: incoming) {
                if(!out[base[predPtr]->index]) { in = false; break; }
            }

            if(in && slot.available.add(block->index)) changed = true;

            auto exit = in || slot.stores[block->index];
            if(exit && out.add(block->index)) changed = true;
        }
    }
}

// Whether an instruction reads the slot, writes it, or both - a copy out of one and into another does
// both, of two different slots, so the two questions are asked separately rather than as a kind.
bool reads(HashMap<U32, U32>& index, LowerInst* inst, Size which) {
    if(inst->kind == LowerInst::Load) return slotOf(index, ((LowerInstLoad*)inst)->from) == which;
    if(inst->kind == LowerInst::Copy) return slotOf(index, ((LowerInstCopy*)inst)->from) == which;
    return false;
}

bool writes(HashMap<U32, U32>& index, LowerInst* inst, Size which) {
    if(inst->kind == LowerInst::Store) return slotOf(index, ((LowerInstStore*)inst)->to) == which;
    if(inst->kind == LowerInst::Copy) return slotOf(index, ((LowerInstCopy*)inst)->to) == which;
    return false;
}

// Which blocks write the slot, and whether any block reads it before anything has written it. The
// second is the only thing that can still disqualify a slot at this point, and it disqualifies it
// completely: a register cannot hold what was never put in it.
bool surveySlot(LowerBase base, LowerFunction& fun, HashMap<U32, U32>& index, Size which, Slot& slot) {
    slot.stores.reset(fun.blocks.size());

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        for(auto instPtr: block->instructions.contents(base)) {
            if(!writes(index, base[instPtr], which)) continue;

            slot.stores.set(block->index, true);
            break;
        }
    }

    computeAvailability(base, fun, slot);

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        U8 written = slot.available[block->index];

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            if(reads(index, inst, which) && !written) return false;
            if(writes(index, inst, which)) written = 1;
        }
    }

    return true;
}

/*
 * The value a load of this slot would have produced from a register holding `stored`.
 *
 * A slot as wide as its register needs nothing, because the load read back every bit that was
 * written. A narrower one is a truncation memory performed and a register does not, so it is
 * performed here - a mask for an unsigned load and the shift pair that sign-extends for a signed one,
 * which are the same two shapes decodePackedField has, for the same reason.
 */
LowerPtr<LowerValue> narrowedTo(LowerBase base, LowerModule& module, LowerBlock& block,
                                LowerPtr<LowerValue> stored, const Slot& slot, bool isSigned,
                                StringId name) {
    auto bits = registerBits(slot.type);
    auto width = slot.width * 8;
    if(width >= bits) return stored;

    if(isSigned) {
        auto shift = new (module.arena) LowerImm(StringId(), slot.type, U64(bits - width));
        block.addInst(base, shift);

        auto up = binary<LowerInst::Shl>(base, module, block, base[stored], shift->created().ptr,
                                         slot.type, StringId());
        return binary<LowerInst::Sar>(base, module, block, up->created().ptr, shift->created().ptr,
                                      slot.type, name)->created().ptr - base;
    }

    auto mask = new (module.arena) LowerImm(StringId(), slot.type, (U64(1) << width) - 1);
    block.addInst(base, mask);

    return binary<LowerInst::And>(base, module, block, base[stored], mask->created().ptr, slot.type,
                                  name)->created().ptr - base;
}

// A phi with room for one alternative per incoming edge. Built detached, because its alternatives are
// what the blocks it merges end up holding and none of them is known yet - adding it to the block is
// what registers those as uses, so that happens once they exist.
LowerInstPhi* makePhi(LowerModule& module, LowerBlock& block, LowerType type) {
    auto count = block.incoming.size();
    auto storage = module.arena.alloc(
        sizeof(LowerInstPhi) +
        sizeof(LowerPtr<LowerValue>) * count +
        sizeof(LowerPtr<LowerBlock>) * count);

    auto phi = new (storage) LowerInstPhi(StringId(), type);
    phi->usedCount = U8(count);
    return phi;
}

/*
 * The rewrite: one pass over each block, dropping the memory traffic and remembering what the slot
 * holds as it goes.
 *
 * The instruction list is rebuilt rather than edited, because a load may need a mask in its place and
 * there is nowhere to put one otherwise - `addInst` appends, and appending during the rebuild is
 * exactly the position the load occupied.
 */
void rewriteBlock(LowerBase base, LowerModule& module, LowerBlock& block, Array<Slot>& slots,
                  HashMap<U32, U32>& index) {
    // Both inline, and both per block: the value each promoted slot holds on entry, and the
    // instructions the rebuild below reads while writing the list it read them from.
    SmallArray<LowerPtr<LowerValue>, 16> current;
    for(auto& slot: slots) current.push(slot.entry[block.index]);

    SmallArray<LowerPtr<LowerInst>, 32> original;
    for(auto instPtr: block.instructions.contents(base)) original.push(instPtr);

    block.instructions.clear();

    for(auto instPtr: original) {
        auto inst = base[instPtr];

        if(inst->kind == LowerInst::Alloca) {
            if(slotOf(index, inst->created().ptr - base) != maxLimit<Size>) {
                detach(base, inst);
                continue;
            }
        } else if(inst->kind == LowerInst::Store) {
            auto store = (LowerInstStore*)inst;
            auto which = slotOf(index, store->to);

            if(which != maxLimit<Size>) {
                current[which] = store->value;
                detach(base, inst);
                continue;
            }
        } else if(inst->kind == LowerInst::Copy) {
            /*
             * A copy with a promoted slot on either end, which is a move between a register and
             * whatever the other end turned out to be: both promoted is an assignment and nothing is
             * emitted, and one promoted becomes the load or the store the copy was already doing.
             *
             * Nothing is masked in either direction. A register that carries bits above the slot's
             * width still reads back correctly, because every *load* of a slot narrows - so the only
             * place the width has to be honoured is where the value leaves, and a store of `width`
             * bytes honours it by being one.
             */
            auto copyInst = (LowerInstCopy*)inst;
            auto to = slotOf(index, copyInst->to);
            auto from = slotOf(index, copyInst->from);

            if(to != maxLimit<Size> || from != maxLimit<Size>) {
                auto destination = copyInst->to;
                auto source = copyInst->from;
                auto& slot = slots[to != maxLimit<Size> ? to : from];

                // Reading a slot that holds nothing is what surveySlot rejects a slot for, and a copy
                // out of one is a read like any other.
                assertTrue(from == maxLimit<Size> || current[from] != nullptr);

                detach(base, inst);

                if(to != maxLimit<Size> && from != maxLimit<Size>) {
                    current[to] = current[from];
                } else if(to != maxLimit<Size>) {
                    current[to] = load(base, module, block, base[source], slot.width, false,
                                       slot.type, StringId())->created().ptr - base;
                } else {
                    block.addInst(base, new (module.arena) LowerInstStore(
                        destination, current[from], slot.width));
                }

                continue;
            }
        } else if(inst->kind == LowerInst::Load) {
            auto loadInst = (LowerInstLoad*)inst;
            auto which = slotOf(index, loadInst->from);

            if(which != maxLimit<Size>) {
                auto& slot = slots[which];
                auto result = loadInst->created().ptr;

                detach(base, inst);

                auto value = narrowedTo(base, module, block, current[which], slot,
                                        loadInst->isSigned(), result->name);
                replaceUses(base, module.arena, result - base, value);
                continue;
            }
        }

        block.instructions.push(module.arena, instPtr);
    }

    for(Size i = 0; i < slots.size(); i++) slots[i].exit[block.index] = current[i];
}

/*
 * A phi that says nothing, removed.
 *
 * Its own value is not an answer - a loop-carried phi whose only other alternative is one value is
 * that value - so self-references are ignored, and what is left has to be a single value for the phi
 * to be redundant. Iterated, because removing one can make the next one trivial, which is how a chain
 * of maximal phis down a straight-line region collapses.
 */
void removeTrivialPhis(LowerBase base, Region<LowerRegion>& arena, Array<LowerPtr<LowerInstPhi>>& phis) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(Size at = 0; at < phis.size(); at++) {
            auto phi = base[phis[at]];
            auto result = ((LowerInstSingle*)phi)->created().ptr;
            auto operands = phi->used();

            LowerPtr<LowerValue> only = nullptr;
            auto trivial = true;

            for(Size i = 0; i < operands.length; i++) {
                auto value = operands.ptr[i];
                if(base[value] == result) continue;

                if(!only) only = value;
                else if(only != value) { trivial = false; break; }
            }

            if(!trivial || !only) continue;

            auto block = base[phi->block];
            detach(base, (LowerInst*)phi);
            replaceUses(base, arena, result - base, only);

            for(Size i = 0; i < block->phis.size(); i++) {
                if(base[block->phis.get(base, i)] == phi) {
                    block->phis.remove(base, i);
                    break;
                }
            }

            phis.remove(at--);
            changed = true;
        }
    }
}

} // namespace

void promoteStackSlots(LowerBase base, LowerFunction& fun) {
    auto& module = *fun.module;

    HashMap<U32, U32> index;
    Array<Slot> candidates;
    collectSlots(base, fun, candidates, index);
    if(candidates.isEmpty()) return;

    /*
     * The survey can reject a slot, and rejecting one changes what the others may be: a load of a
     * rejected slot is an ordinary load again, and the index has to stop claiming its address. So the
     * accepted set is rebuilt rather than filtered in place, and the index with it.
     */
    Array<Slot> slots;
    for(Size i = 0; i < candidates.size(); i++) {
        if(surveySlot(base, fun, index, i, candidates[i])) slots.push(::move(candidates[i]));
    }

    if(slots.isEmpty()) return;

    index.clear();
    for(Size i = 0; i < slots.size(); i++) index.add(U32(slots[i].address), U32(i));

    // A phi wherever the slot arrives already written, which is every block a value has to be merged
    // into and a good many where it does not - see removeTrivialPhis.
    Array<LowerPtr<LowerInstPhi>> phis;
    for(auto& slot: slots) {
        for(Size i = 0; i < fun.blocks.size(); i++) {
            slot.entry.push(nullptr);
            slot.exit.push(nullptr);
        }
    }

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        for(auto& slot: slots) {
            if(!slot.available[block->index]) continue;

            auto phi = makePhi(module, *block, slot.type);
            slot.entry[block->index] = ((LowerInstSingle*)phi)->created().ptr - base;
            phis.push(phi - base);
        }
    }

    for(auto blockPtr: fun.blocks.contents(base)) {
        rewriteBlock(base, module, *base[blockPtr], slots, index);
    }

    // Nothing may still be holding the address of storage that no longer exists. An instruction left
    // referring to a deleted one is not caught downstream - the validator asks which *function* a
    // value belongs to rather than whether its instruction is still in a block - so it is asked here,
    // where the answer identifies which use was missed.
    for(auto& slot: slots) assertTrue(base[slot.address]->uses.isEmpty());

    // The alternatives, now that every block has said what it holds on the way out, and the phis
    // themselves - adding one is what puts it in its block and registers those alternatives as uses.
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        for(auto& slot: slots) {
            auto entry = slot.entry[block->index];
            if(!entry) continue;

            auto phi = (LowerInstPhi*)base[entry]->inst();
            auto operands = phi->used();
            auto sources = phi->sources();

            Size at = 0;
            for(auto predPtr: block->incoming.contents(base)) {
                operands.ptr[at] = slot.exit[base[predPtr]->index];
                sources.ptr[at] = predPtr;
                at++;
            }

            block->addInst(base, (LowerInst*)phi);
        }
    }

    removeTrivialPhis(base, module.arena, phis);
}
