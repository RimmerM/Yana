#include "lower_promote.h"
#include "lower_inst.h"
#include "lower_builder.h"

/*
 * Promotion, as the textbook algorithm with one deliberate simplification.
 *
 * The standard construction places a phi exactly where a slot's definitions meet - the iterated
 * dominance frontier of the blocks that write it. This walks the blocks in reverse postorder
 * instead, so that what a block carries in is already known from its predecessors: one of them, or
 * several that agree, is that value directly, and only a disagreement or an unvisited predecessor -
 * a back edge, so a loop header - is a phi. The two produce the same IR, and this one needs no
 * dominance frontier and no dominator-tree walk.
 *
 * It used to place one in *every* block the slot arrives already written into and delete the ones
 * whose alternatives agreed afterwards, which is the same answer by a route whose cost is the number
 * of slots times the number of blocks. See the note above `promoteStackSlots`, where a single
 * inlined body made that product 818,460.
 *
 * What it does need is to know where a phi would have nothing to say. A slot that has not been
 * written on every path into a block has no value to carry there, so "written on every path" is a
 * plain forward AND-dataflow computed first, and it decides three things at once: where a phi may be
 * placed, that every alternative of one exists, and whether any load reads a slot that was never
 * written - which is the one shape that makes a slot unpromotable rather than merely unpromoted.
 */

namespace {

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

    // Which candidate this was while the survey ran, and therefore which column of its
    // availability answers for this slot - see `surveySlots`, which keeps that table per block
    // rather than per slot and so is not renumbered when a slot is dropped.
    U32 candidate = 0;

    // Per block, indexed by LowerBlock::index.
    Array<LowerPtr<LowerValue>> entry;   // the phi carrying it in, where one was placed
    Array<LowerPtr<LowerValue>> exit;    // what it holds on the way out, or null
};

/*
 * One phi this pass placed, the block it belongs to and the slot it carries.
 *
 * Kept beside the phi rather than looked back out of `Slot::entry`, because that array no longer
 * names a phi everywhere it holds a value: a block whose predecessors all hand it the same thing
 * carries that value in directly and has nothing to fill in. So the fill below walks the phis that
 * were placed instead of the blocks that have a value, and `removeTrivialPhis` walks the same list.
 */
struct PlacedPhi {
    LowerPtr<LowerInstPhi> phi = nullptr;
    LowerPtr<LowerBlock> block = nullptr;
    U32 slot = 0;
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
 * Whether a value held in one register can be moved into the other, which is the whole of what a
 * `copy` between two promoted slots turns into: the same bits, read out of a different register.
 *
 * Stated as the set of moves a machine has rather than as a rule about types, because that is what
 * it is - `validateBitcast` says the same thing from the other side, and the assertions in
 * `machine_select.cpp` are what a wrong answer here trips.
 */
bool carriesInto(LowerType from, LowerType to) {
    if(from == to) return true;

    // Two vectors are one register read at another lane shape, and no instruction at all - but only
    // where they are the same register. A mask is its own shape, and a different lane count is a
    // different number of truth values rather than a renaming.
    if(isVectorLike(from) || isVectorLike(to)) {
        if(!isVectorLike(from) || !isVectorLike(to)) return false;
        if(from.isMask() != to.isMask()) return false;
        if(from.isMask() && from.laneShift != to.laneShift) return false;

        return from.byteWidth() == to.byteWidth();
    }

    // Within the integer bank it is a `mov` whatever the two widths are. Across the banks it is
    // MOVD or MOVQ, and which of the two it is is decided by a width the two ends have to share.
    if(isIntLike(from) && isIntLike(to)) return true;
    return registerBits(from) == registerBits(to);
}

/*
 * The one constraint a slot's own accesses cannot state, and the one this pass was missing.
 *
 * `collectSlots` reads a slot's register type off its loads and its stores, and a `copy` has neither
 * - it names two addresses and a byte count, and says nothing about what shape either end is read
 * at. But a copy *between two promoted slots* is a write of the destination's register from the
 * source's, so the two types meet in the rewrite whether or not either access mentioned the other:
 * a `Result(FileError, ())` built in two temporaries and copied into one local is the shape, where
 * the niche constant arrives at the record's width and the callee's own result at its narrower one.
 * The phi that merged them then had alternatives of two different types, which the validator refuses
 * - correctly, and with no location, which is the worst way to find out.
 *
 * Where the two registers can hold each other the rewrite emits the `bitcast` that moves between
 * them and both slots are kept. Where they cannot - a float against an integer of another width,
 * two vectors of different widths - the destination is dropped instead, and the copy goes back to
 * being the store it always was. Dropping the *destination* rather than the source is arbitrary
 * between two slots and not between more: a chain of copies is broken wherever it is cut.
 *
 * Iterated, because the index every copy is looked up in is rebuilt by a drop.
 */
void reconcileCopiedSlots(LowerBase base, LowerFunction& fun, Array<Slot>& slots,
                          HashMap<U32, U32>& index) {
    for(;;) {
        auto reject = maxLimit<Size>;

        for(auto blockPtr: fun.blocks.contents(base)) {
            for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
                auto inst = base[instPtr];
                if(inst->kind != LowerInst::Copy) continue;

                auto copyInst = (LowerInstCopy*)inst;
                auto to = slotOf(index, copyInst->to);
                auto from = slotOf(index, copyInst->from);

                if(to == maxLimit<Size> || from == maxLimit<Size>) continue;
                if(carriesInto(slots[from].type, slots[to].type)) continue;

                reject = to;
                break;
            }

            if(reject != maxLimit<Size>) break;
        }

        if(reject == maxLimit<Size>) return;

        slots.remove(reject);

        index.clear();
        for(Size i = 0; i < slots.size(); i++) index.add(U32(slots[i].address), U32(i));
    }
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

            /*
             * A slot of a size no register holds is not one. Which sizes those are depends on what
             * the slot turns out to hold - a vector register is wider than any scalar one - and the
             * type is not known until the users below have been walked, so the widths of *both*
             * kinds pass here and `holdableWidth` decides once there is a type to ask about.
             */
            auto width = U32(((LowerImm*)sizeInst)->i);
            if(width != 1 && width != 2 && width != 4 && width != 8 &&
               width != 16 && width != 32 && width != 64) {
                continue;
            }

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

            // And now that there is a type, whether a register of that kind holds this many bytes,
            // and whether the storage is the width the register reads back whole - see
            // `promotableSlot` in lower_promote.h, which `splitAggregateSlots` asks the same
            // question of a field it is deciding how to move.
            if(!promotableSlot(width, slot.type)) continue;

            index.add(U32(slot.address), U32(into.size()));
            into.push(::move(slot));
        }
    }

    reconcileCopiedSlots(base, fun, into, index);
}

// Which slot an instruction reads and which it writes - a copy out of one and into another does
// both, of two different slots, so the two questions are asked separately rather than as a kind.
// `maxLimit<Size>` for an instruction that does neither, which is most of them.
Size readSlot(HashMap<U32, U32>& index, LowerInst* inst) {
    if(inst->kind == LowerInst::Load) return slotOf(index, ((LowerInstLoad*)inst)->from);
    if(inst->kind == LowerInst::Copy) return slotOf(index, ((LowerInstCopy*)inst)->from);
    return maxLimit<Size>;
}

Size writtenSlot(HashMap<U32, U32>& index, LowerInst* inst) {
    if(inst->kind == LowerInst::Store) return slotOf(index, ((LowerInstStore*)inst)->to);
    if(inst->kind == LowerInst::Copy) return slotOf(index, ((LowerInstCopy*)inst)->to);
    return maxLimit<Size>;
}

/*
 * Where every slot is known to hold something, and which slots are still worth promoting - the whole
 * survey, over one walk of the function rather than one walk per slot.
 *
 * The transpose is the point. Each of the three answers below used to be computed per slot, and each
 * of them costs a walk of something the size of the function: the blocks that write it, the
 * availability fixpoint over the CFG, and the check that nothing reads it before anything wrote it.
 * A body with a slot per line therefore paid the function's size twice over, and an inlined test
 * case of 34,000 instructions with 1,320 slots spent 0.7s here. Held per *block* instead - one bit
 * per slot in a row per block - all three fall out of a single pass, and the dataflow is a word of
 * slots at a time rather than a set per slot.
 *
 * ## The dataflow
 *
 * `in[b] = AND over predecessors of out[p]` and `out[b] = in[b] || writes`, solved from the
 * *optimistic* end: every block starts "written" and the fixpoint takes away the ones that are not.
 * A block nothing jumps to - the entry, and anything unreachable - has no predecessors and so is
 * denied on the first pass, which is what makes an unreachable predecessor deny a phi rather than
 * leave it an alternative that does not exist.
 *
 * The direction matters, and starting from the pessimistic end is wrong rather than conservative. An
 * `in` that is the AND over predecessors cannot become true inside a loop unless it was already true
 * when the latch was first looked at, so "false everywhere" is a fixpoint of every cycle whose blocks
 * do not all write the slot. What that cost was every local live across a *nested* loop: the outer
 * loop's latch writes `row` and not `total`, so `total` was never available at the block that reads
 * it, and both stayed in memory. One loop deep happened to work - there the latch is the body, and
 * the body writes what it carries - which is why this went unnoticed. The greatest fixpoint is the
 * answer a must-analysis wants and the optimistic start is what reaches it.
 *
 * ## What can still disqualify a slot
 *
 * A read of storage nothing has written on every path into it, and that is the only thing left at
 * this point - a register cannot hold what was never put in it. It disqualifies the slot completely
 * rather than leaving the read unanswered, and it is decided per slot out of the same walk that
 * carries every other slot's state, since "written by here" is a bit in a row like the rest.
 *
 * Rejecting one slot says nothing about any other: every question here is asked of one address, and
 * a load of a rejected slot is an ordinary load rather than an event in another slot's dataflow. So
 * the answers computed for the survivors stand as they are and none of this is redone.
 */
void surveySlots(LowerBase base, LowerFunction& fun, HashMap<U32, U32>& index,
                 Array<Slot>& candidates, IndexSetList& available, IndexSet& usable) {
    auto blockCount = fun.blocks.size();
    auto slotCount = candidates.size();

    usable.reset(slotCount);
    usable.fill();

    IndexSetList stores;
    stores.reset(blockCount, slotCount);

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        for(auto instPtr: block->instructions.contents(base)) {
            auto which = writtenSlot(index, base[instPtr]);
            if(which != maxLimit<Size>) stores[block->index].set(which, true);
        }
    }

    IndexSetList out;
    available.reset(blockCount, slotCount);
    out.reset(blockCount, slotCount);

    for(Size i = 0; i < blockCount; i++) {
        available[i].fill();
        out[i].fill();
    }

    IndexSet incoming;
    incoming.reset(slotCount);

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];
            auto edges = block->incoming.size();

            if(edges == 0) incoming.reset(slotCount);
            else {
                incoming.copyFrom(out[base[block->incoming.get(base, 0)]->index]);
                for(Size i = 1; i < edges; i++) {
                    incoming.intersectWith(out[base[block->incoming.get(base, i)]->index]);
                }
            }

            if(!available[block->index].equals(incoming)) {
                available[block->index].copyFrom(incoming);
                changed = true;
            }

            incoming.unionWith(stores[block->index]);

            if(!out[block->index].equals(incoming)) {
                out[block->index].copyFrom(incoming);
                changed = true;
            }
        }
    }

    IndexSet written;

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        written.copyFrom(available[block->index]);

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            auto read = readSlot(index, inst);
            if(read != maxLimit<Size> && !written[read]) usable.set(read, false);

            auto write = writtenSlot(index, inst);
            if(write != maxLimit<Size>) written.set(write, true);
        }
    }
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

/*
 * The value one promoted slot holds, in the register another one's type names.
 *
 * The same bits either way - what changes is which register they are read out of - so it is a
 * `bitcast` and never a `cast`: a `Result(FileError, ())` whose niche constant is a 64-bit literal
 * and whose payload is a 32-bit call result is one byte of storage read two ways, and converting it
 * would be converting a number that was never one. `carriesInto` has already answered that a move
 * between these two registers exists.
 *
 * Nothing is masked here for the reason nothing is masked at the copy this replaces: a register
 * carrying bits above the slot's width still reads back correctly, because every load narrows.
 */
LowerPtr<LowerValue> carriedInto(LowerBase base, LowerModule& module, LowerBlock& block,
                                 LowerPtr<LowerValue> value, LowerType type) {
    if(base[value]->type == type) return value;

    auto cast = block.addInst(base, new (module.arena) LowerInstUnary(
        LowerInst::Bitcast, StringId(), type, value));

    return ((LowerInstSingle*)cast)->created().ptr - base;
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

    /*
     * Zeroed rather than left as the arena found it, which is not tidiness.
     *
     * "Detached" above means it is not in a block and holds no uses. It does *not* mean nothing can
     * reach it: `Slot::entry` names this phi as what a block holds on entry from the moment it is
     * made, so the rewrite below builds instructions that read it, and an analysis walking those
     * operands arrives here while the alternatives are still whatever the arena last had.
     *
     * `knownZeroBits` is the one that walks them, and a null alternative is what tells it this phi
     * is not answerable yet - which is the honest answer, since the alternatives are what would
     * decide it. Reading uninitialized handles instead is a wild pointer, and it was one.
     */
    auto used = phi->used();
    for(Size i = 0; i < used.length; i++) used.ptr[i] = nullptr;

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
             * whatever the other end turned out to be: both promoted is an assignment, and at most
             * the `bitcast` that reads one register as the other's type - see carriedInto - while
             * one promoted becomes the load or the store the copy was already doing.
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
                    current[to] = carriedInto(base, module, block, current[from], slots[to].type);
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
 * to be redundant.
 *
 * A worklist rather than a sweep repeated until nothing changes, and the difference is the whole
 * cost of this function. Removing a phi can only make one of *its own readers* trivial, so the
 * readers are what goes back on the list; the sweep it replaces rescanned every surviving phi to
 * find them, once per removal, and shifted the array down by one on top of that. Both are linear in
 * the number of phis and both ran per phi, which is where a body large enough to have thousands
 * spent effectively all of its compile time - see the note above `promoteStackSlots`.
 */
void removeTrivialPhis(LowerBase base, Region<LowerRegion>& arena, Array<PlacedPhi>& placed) {
    // Which entry a phi's result is, so that a reader found through the use list can be put back on
    // the worklist. Only phis this pass placed are in it; a phi from anywhere else reading one of
    // these is not something removing it can make trivial, since its alternatives were already
    // whatever they were.
    HashMap<U32, U32> byResult;
    for(Size i = 0; i < placed.size(); i++) {
        auto result = ((LowerInstSingle*)base[placed[i].phi])->created().ptr - base;
        byResult.add(U32(result), U32(i));
    }

    IndexSet gone;
    gone.reset(placed.size());

    Array<U32> work;
    for(Size i = placed.size(); i > 0; i--) work.push(U32(i - 1));

    while(work.size()) {
        auto at = Size(work.pop().unwrap());
        if(gone[at]) continue;

        auto phi = base[placed[at].phi];
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

        // The readers, collected before the replacement moves them off this value's use list.
        SmallArray<U32, 8> readers;
        for(auto userPtr: result->uses.contents(base)) {
            auto user = base[userPtr];
            if(user->kind != LowerInst::Phi) continue;

            auto found = byResult.getValue(U32(((LowerInstSingle*)user)->created().ptr - base));
            if(found) readers.push(found.unwrap());
        }

        auto block = base[phi->block];
        detach(base, (LowerInst*)phi);
        replaceUses(base, arena, result - base, only);

        for(Size i = 0; i < block->phis.size(); i++) {
            if(base[block->phis.get(base, i)] == phi) {
                block->phis.remove(base, i);
                break;
            }
        }

        gone.set(at, true);
        for(auto reader: readers) {
            if(!gone[reader]) work.push(reader);
        }
    }
}

} // namespace

void promoteStackSlots(LowerBase base, LowerFunction& fun) {
    auto& module = *fun.module;

    /*
     * The traversal order the placement below wants, taken first because taking it renumbers.
     *
     * `buildPostorder` writes each block's position in the list back into `index` on its way past,
     * and every set in this pass - `stores`, `available` - is indexed by that number. Asking for the
     * order before any of them is built is what keeps the two agreeing; the alternative is a pass
     * whose correctness rests on the block list having been renumbered by somebody else.
     */
    auto postorder = fun.buildPostorder(base);

    HashMap<U32, U32> index;
    Array<Slot> candidates;
    collectSlots(base, fun, candidates, index);
    if(candidates.isEmpty()) return;

    /*
     * The survey can reject a slot, and rejecting one changes what the others may be: a load of a
     * rejected slot is an ordinary load again, and the index has to stop claiming its address. So the
     * accepted set is rebuilt rather than filtered in place, and the index with it.
     *
     * `available` is not rebuilt with it. It is a row per block over the *candidate* numbering, which
     * a rejection does not disturb, so each surviving slot carries the column it had - see
     * `Slot::candidate`.
     */
    IndexSetList available;
    IndexSet usable;
    surveySlots(base, fun, index, candidates, available, usable);

    Array<Slot> slots;
    for(Size i = 0; i < candidates.size(); i++) {
        if(!usable[i]) continue;

        candidates[i].candidate = U32(i);
        slots.push(::move(candidates[i]));
    }

    if(slots.isEmpty()) return;

    index.clear();
    for(Size i = 0; i < slots.size(); i++) index.add(U32(slots[i].address), U32(i));

    for(auto& slot: slots) {
        for(Size i = 0; i < fun.blocks.size(); i++) {
            slot.entry.push(nullptr);
            slot.exit.push(nullptr);
        }
    }

    /*
     * A phi only where the predecessors disagree, which is what a phi is for.
     *
     * This used to place one in *every* block a slot arrives already written into and let
     * `removeTrivialPhis` take back the ones whose alternatives all agree - the simplification the
     * header comment describes, on the grounds that the functions this runs over are small. That
     * holds until one of them is not. The phi count is the product of two numbers that both grow
     * with the body, so an inlined test case of 34,000 instructions had 1,320 slots over 1,242
     * blocks and this built **818,460 phis** to delete all but a handful of them again. Building
     * them cost 22 ms; taking them back cost fourteen seconds, and was the whole of the quartic
     * term - see test/bench/findings.md §63.
     *
     * What replaces it needs no dominance frontier either. Walking the blocks in reverse postorder
     * means a block's predecessors have already said what they leave the slot holding, so the answer
     * to "what does this block carry in" is usually there to be read: one predecessor, or several
     * that agree, is that value and no instruction at all. Only a genuine disagreement, and a
     * predecessor not yet visited - which is a back edge, so a loop header - gets a phi.
     *
     * That is the same set of phis the sweep left behind, arrived at without making the others
     * first. `removeTrivialPhis` still runs, and one shape still reaches it in bulk: a loop header
     * has a predecessor the walk has not been to, so it gets a phi whether or not anything in the
     * loop writes the slot, and only the back edge's value can say which. That leaves a phi per slot
     * per loop header - a product again, one term of which is much smaller. Removing it is what the
     * iterated dominance frontier would do, since a slot with no store inside the loop is not in it;
     * it is not done here because what it would cost is a dominance frontier per function against a
     * term that is now a sixth of the pass rather than all of it.
     *
     * Forwarding a predecessor's value is only sound because it dominates: every path to this block
     * runs through one of the predecessors, each of those is dominated by the value's definition, so
     * the block is too. That is the standard argument for the construction and it is why the
     * agreement has to be over *all* the incoming edges rather than over the ones already visited.
     */
    auto blockList = fun.blocks.contents(base);

    // Whether a block has been rewritten, and therefore whether `exit` says anything about it. Not
    // the same question as "earlier in the walk": a block nothing reaches is not in the postorder at
    // all, and is visited afterwards with nothing assumed about its predecessors.
    IndexSet settled;
    settled.reset(fun.blocks.size());

    Array<PlacedPhi> placed;

    auto enter = [&](LowerBlock* block, bool ordered) {
        for(Size i = 0; i < slots.size(); i++) {
            auto& slot = slots[i];
            if(!available[block->index][slot.candidate]) continue;

            LowerPtr<LowerValue> only = nullptr;
            auto agreed = ordered;

            if(ordered) {
                for(auto predPtr: block->incoming.contents(base)) {
                    auto pred = base[predPtr];
                    if(!settled[pred->index]) { agreed = false; break; }

                    auto value = slot.exit[pred->index];
                    if(!value) { agreed = false; break; }

                    if(!only) only = value;
                    else if(only != value) { agreed = false; break; }
                }
            }

            // Availability is the statement that every predecessor left something here, so the null
            // above is unreachable; it is tested rather than asserted because what a wrong answer
            // would cost is a phi rather than a value nothing wrote.
            if(agreed && only) {
                slot.entry[block->index] = only;
                continue;
            }

            auto phi = makePhi(module, *block, slot.type);
            slot.entry[block->index] = ((LowerInstSingle*)phi)->created().ptr - base;
            placed.push(PlacedPhi { phi - base, block - base, U32(i) });
        }

        rewriteBlock(base, module, *block, slots, index);
        settled.set(block->index, true);
    };

    for(Size i = postorder.size(); i > 0; i--) enter(base[blockList[postorder[i - 1]]], true);

    // And then whatever the traversal did not reach. Such a block still holds accesses to storage
    // that is about to go, so it is rewritten like any other - what it is not is a block whose
    // predecessors say anything, so it takes the phi it would have had before.
    for(auto blockPtr: blockList) {
        if(!settled[base[blockPtr]->index]) enter(base[blockPtr], false);
    }

    // Nothing may still be holding the address of storage that no longer exists. An instruction left
    // referring to a deleted one is not caught downstream - the validator asks which *function* a
    // value belongs to rather than whether its instruction is still in a block - so it is asked here,
    // where the answer identifies which use was missed.
    for(auto& slot: slots) assertTrue(base[slot.address]->uses.isEmpty());

    // The alternatives, now that every block has said what it holds on the way out, and the phis
    // themselves - adding one is what puts it in its block and registers those alternatives as uses.
    for(auto& entry: placed) {
        auto phi = base[entry.phi];
        auto block = base[entry.block];
        auto& slot = slots[entry.slot];

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

    removeTrivialPhis(base, module.arena, placed);
}
