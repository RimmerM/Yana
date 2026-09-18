#include "transform_internal.h"

/*
 * `loadUpTo` and `storeUpTo` - `LowerInst::VecLoadUpTo` and `VecStoreUpTo`, Analysis-Avx512.md §3.5.
 *
 * A vector transfer that stops at a lane count, and touches no byte past it. Three routes, chosen by
 * what the level has:
 *
 *  - **`avx512`, every lane.** A masked move to or from memory under a mask register, which faults on
 *    no lane its mask clears: `vmovdqu8`..`64` or `vmovups`/`vmovupd` with `{k}`. The mask is `bzhi` of
 *    all ones at the count, moved into `k` - the same two instructions `maskUpTo` takes there - with
 *    the count clamped into what `bzhi` reads first, unless its known bits already keep it there.
 *  - **`avx2`, four- and eight-byte lanes.** `vpmaskmovd`/`q`, whose mask is a vector with each lane's
 *    sign bit set where the lane moves: `iota .< splat(count)` at the lane's own width.
 *  - **Everything else** - every lane at `sse4`, and bytes and words at `avx2` - has no instruction,
 *    and is `blockShort`'s ladder (lib/Native/Memory.native.yana) written through a frame slot: the
 *    whole vector where the count covers it, and otherwise overlapping pairs of transfers at the
 *    widest power of two below the byte count, with no rung for a count at or below zero. The load
 *    fills a zeroed slot and reads it back; the store spills the vector and copies out of it.
 *
 * The ladder needs no clamp at all, which is why only the other two routes have one: the whole-vector
 * test and every rung's test are signed, so a count at or above the lane count takes the whole
 * vector and one at or below zero reaches the bottom of the tree having taken nothing.
 *
 * ## Where it sits
 *
 * Below `chooseMaskDomain`, `lowerMaskRegisters` and `fuseMaskRegisters`, so that the mask this
 * builds is already in the bank it will stay in - a mask register at `avx512`, where nothing is left
 * to demote it, and a vector at `avx2`. Above `poolVectorConstants`, which turns the `avx2` route's
 * lane numbers and the ladder's zero into what they cost; and above `selectAddressesAndLeas`, which
 * folds each transfer's address operand the way it folds a load's.
 */

namespace {

enum class Route { MaskRegister, MaskVector, Ladder };

Route routeFor(LowerType type) {
    if(hasEvexRow(EvexRow::MaskedMove)) return Route::MaskRegister;

    auto width = laneBytes(type.lane);
    if((targetFeatures() & kFeatureAvx2) && (width == 4 || width == 8)) return Route::MaskVector;

    return Route::Ladder;
}

// The transfer being rewritten, with its three operands read out before the instruction goes.
struct Transfer {
    bool store;
    LowerType type;      // the whole vector
    LowerValue* address;
    LowerValue* count;   // lanes, as an `Int64`
    LowerValue* value;   // the store's vector, or null
};

// A count of any integer width as an `Int64`, signed - it is a lane count, and a negative one means
// none.
LowerValue* countAsInt64(Expansion& e, LowerValue* count) {
    if(count->type == LowerType::Int64) return count;
    return e.convert(LowerType::Int64, count, true, true);
}

// A constant count as the signed number it is at the width it was written, so that a negative
// thirty-two-bit one stays negative.
I64 constantCount(LowerValue* count) {
    auto value = immValue(count);
    return count->type == LowerType::Int64 ? I64(value) : I64(I32(U32(value)));
}

/*
 * The mask register a transfer at `avx512` moves under: `bzhi` of all ones at the count.
 *
 * `bzhi` reads the low byte of its index and keeps every bit where that byte is at or above the
 * register's width, so the index has to be within 0..255 and nothing more: a lane count at or above
 * the lanes is every lane either way, since a mask register holds nothing above its lanes. A count
 * that could be negative is taken to zero, and one that could pass the byte to the lane count, each
 * by a comparison and a conditional move - and neither where the count's known bits rule it out,
 * which is what a count computed as `n and (width - 1)` or a small constant gives.
 */
LowerValue* maskRegisterUpTo(Expansion& e, LowerValue* count, LowerType type) {
    auto lanes = U64(type.lanes());
    auto scalar = LowerType::Int64;
    auto mask = maskType(type.lane, type.lanes());

    // Thirty-two bits of `bzhi` and `kmovd` cover every mask a whole register has until the 512-bit
    // forms exist (Analysis-Avx512.md step 10), where a byte vector has sixty-four lanes.
    assertTrue(lanes <= 32);

    // A constant count between none and all - the two ends never reach here - is a constant mask.
    if(count->inst()->kind == LowerInst::Imm) {
        auto bits = (U64(1) << immValue(count)) - 1;
        return e.emit(new (e.fun.arena) LowerInstX86MaskConvert(StringId(), mask,
                                                                  e.integer(LowerType::Int32, bits) - e.base, false));
    }

    auto known = knownZeroBits(e.base, count);
    auto index = count;

    if(!(known & (U64(1) << 63))) {
        auto negative = e.compare(LowerCmp::ilt, index, e.integer(scalar, 0));
        index = e.select(scalar, negative, e.integer(scalar, 0), index);
    }

    auto aboveByte = ~U64(255);
    if((known & aboveByte) != aboveByte) {
        auto within = e.compare(LowerCmp::lt, index, e.integer(scalar, lanes));
        index = e.select(scalar, within, index, e.integer(scalar, lanes));
    }

    auto narrow = e.convert(LowerType::Int32, index, false, false);
    auto bits = e.intrinsic2(LowerIntrinsic::Bzhi, LowerType::Int32, e.integer(LowerType::Int32, 0xffffffff), narrow);

    return e.emit(new (e.fun.arena) LowerInstX86MaskConvert(StringId(), mask, bits - e.base, false));
}

/*
 * The vector mask a transfer at `avx2` moves under: lane `i < count`, compared at the lane's own
 * integer width so that the mask's lanes are the vector's.
 *
 * A quadword lane takes the count as it is - the comparison is signed and sixty-four bits wide, so
 * every count answers what it means. A doubleword lane truncates it first, and that is where a clamp
 * is owed: into 0..lanes, unless the known bits already keep it inside thirty-one.
 */
LowerValue* maskVectorUpTo(Context& ctx, Expansion& e, LowerValue* count, LowerType type) {
    auto lanes = type.lanes();
    auto quad = laneBytes(type.lane) == 8;
    auto indexType = vectorType(quad ? LowerLane::Int64 : LowerLane::Int32, lanes);
    auto bound = count;

    if(!quad) {
        auto known = knownZeroBits(e.base, count);
        auto aboveInt = ~U64(0x7fffffff);

        if((known & aboveInt) != aboveInt) {
            auto negative = e.compare(LowerCmp::ilt, bound, e.integer(LowerType::Int64, 0));
            bound = e.select(LowerType::Int64, negative, e.integer(LowerType::Int64, 0), bound);

            auto within = e.compare(LowerCmp::lt, bound, e.integer(LowerType::Int64, lanes));
            bound = e.select(LowerType::Int64, within, bound, e.integer(LowerType::Int64, lanes));
        }

        bound = e.convert(LowerType::Int32, bound, false, false);
    }

    U8 bytes[kMaxVectorBytes] = {};
    auto width = laneBytes(indexType.lane);
    for(U32 i = 0; i < lanes; i++) bytes[i * width] = U8(i);

    auto pooled = pooledBytes(ctx, *e.fun.module, bytes, indexType.byteWidth());
    auto address = e.emit(new (e.fun.arena) LowerInstGlobal(StringId(), pooled - *e.fun.module->arena));
    auto iota = e.load(indexType, address, indexType.byteWidth());

    return e.emit(new (e.fun.arena) LowerInstCmp(StringId(), iota - e.base,
                                                   e.splat(indexType, bound) - e.base, LowerCmp::ilt,
                                                   maskType(indexType.lane, lanes)));
}

LowerBlock* newBlock(LowerBase base, LowerFunction& fun, LowerBlock* near) {
    auto block = new (fun.arena) LowerBlock(near->fun, StringId(), BlockIndex(fun.blocks.size()));
    fun.blocks.push(fun.arena, block - base);
    block->source = near->source;
    return block;
}

/*
 * The ladder: `bytes` bytes copied from `from` to `to`, for a `bytes` below the vector's width that
 * may be anything down to a negative number, ending at `end`.
 *
 * `rungs` are the transfer widths from the widest down, each a power of two at least a lane wide and
 * below the vector. A rung of width `r` moves every count from `r` up to `2r - 1` as two transfers of
 * `r` bytes, one at each end, which overlap in the middle - both reads before either write, so the
 * two addresses may be anything. Written as a tree, as `blockShort` is: each test halves what is
 * left, so no count passes more than three of them - five rungs and "none" are six outcomes, and the
 * split rounds up so that the half which also holds "none" is the smaller one.
 *
 * `known` says the count is at least the narrowest rung in range, so that the last one needs no
 * test of its own.
 */
struct Ladder {
    LowerBase base;
    LowerFunction& fun;
    LowerModule& module;
    LowerBlock* near;
    LowerValue* to;
    LowerValue* from;
    LowerValue* bytes;
    LowerBlock* end;
    U32 rungs[5];
    U32 rungCount;

    // A narrow scalar is an `Int32` moved at its own width, as every access below a word is here.
    static LowerType wordFor(U32 width) {
        if(width <= 4) return LowerType::Int32;
        return width == 8 ? LowerType::Int64 : vectorType(LowerLane::Int8, width);
    }

    // One rung: both ends read, then both written. A byte is one transfer, the count being exactly
    // one wherever it reaches here.
    void transfer(LowerBlock* block, U32 width) {
        Expansion e { base, fun, block, block->instructions.size() };
        auto type = wordFor(width);

        if(width == 1) {
            e.store(to, e.load(type, from, 1), 1);
        } else {
            auto tail = e.binary(LowerInst::Sub, LowerType::Int64, bytes, e.integer(LowerType::Int64, width));
            auto low = e.load(type, from, width);
            auto high = e.load(type, e.binary(LowerInst::Add, LowerType::Pointer, from, tail), width);

            e.store(to, low, width);
            e.store(e.binary(LowerInst::Add, LowerType::Pointer, to, tail), high, width);
        }

        jmp(base, module, *block, end);
    }

    // The branch on `bytes >= width` at the end of `block`.
    void test(LowerBlock* block, U32 width, LowerBlock* then, LowerBlock* otherwise) {
        Expansion e { base, fun, block, block->instructions.size() };
        auto holds = e.compare(LowerCmp::ige, bytes, e.integer(LowerType::Int64, width));
        je(base, module, *block, holds, then, otherwise);
    }

    void tree(LowerBlock* block, U32 first, U32 last, bool known) {
        if(last - first == 1) {
            if(known) {
                transfer(block, rungs[first]);
                return;
            }

            auto taken = newBlock(base, fun, near);
            test(block, rungs[first], taken, end);
            transfer(taken, rungs[first]);
            return;
        }

        auto middle = (first + last + 1) / 2;
        auto upper = newBlock(base, fun, near);
        auto lower = newBlock(base, fun, near);

        test(block, rungs[middle - 1], upper, lower);
        tree(upper, first, middle, true);
        tree(lower, middle, last, known);
    }
};

void rewriteByLadder(LowerBase base, LowerFunction& fun, LowerBlock* block, Size at, LowerInst* inst,
                     const Transfer& transfer) {
    auto& module = *fun.module;
    auto type = transfer.type;
    auto width = type.byteWidth();
    auto lane = laneBytes(type.lane);

    // Everything the tests read is computed above the cut, where it dominates every block below.
    Expansion e { base, fun, block, at };
    auto whole = e.compare(LowerCmp::ige, transfer.count, e.integer(LowerType::Int64, type.lanes()));
    auto shift = lane == 1 ? 0 : lane == 2 ? 1 : lane == 4 ? 2 : 3;
    auto bytes = shift ? e.binary(LowerInst::Shl, LowerType::Int64, transfer.count, e.integer(LowerType::Int64, shift))
                       : transfer.count;

    auto after = splitBlockAfter(base, fun, block, e.at);
    removeInst(base, inst);

    auto wholeBlock = newBlock(base, fun, block);
    auto partBlock = newBlock(base, fun, block);
    je(base, module, *block, whole, wholeBlock, partBlock);

    auto zeroBytes = vectorType(LowerLane::Int8, width);

    Expansion part { base, fun, partBlock, 0 };
    auto slot = part.emit(new (fun.arena) LowerInstAlloca(StringId(), part.integer(LowerType::Int64, width) - base, 8));

    // The load's slot starts at zero, which is what the lanes past the count answer; the store's
    // holds the vector, which is what the rungs copy out of.
    if(transfer.store) {
        part.store(slot, transfer.value, width);
    } else {
        part.store(slot, part.splat(zeroBytes, part.integer(LowerType::Int32, 0)), width);
    }

    // Where the rungs end: `after` itself for the store, and for the load a block that reads the
    // slot back before it goes there.
    auto join = transfer.store ? after : newBlock(base, fun, block);

    Ladder ladder { base, fun, module, block,
                    transfer.store ? transfer.address : slot, transfer.store ? slot : transfer.address,
                    bytes, join, {}, 0 };

    for(U32 rung = 16; rung >= lane; rung /= 2) {
        if(rung < width) ladder.rungs[ladder.rungCount++] = rung;
    }

    ladder.tree(partBlock, 0, ladder.rungCount, false);

    Expansion full { base, fun, wholeBlock, 0 };

    if(transfer.store) {
        full.store(transfer.address, transfer.value, width);
        jmp(base, module, *wholeBlock, after);
        return;
    }

    auto loaded = full.load(type, transfer.address, width);
    jmp(base, module, *wholeBlock, after);

    Expansion back { base, fun, join, 0 };
    auto partial = back.load(type, slot, width);
    jmp(base, module, *join, after);

    auto phi = makePhi(fun.arena, type, 2);
    phi->used().ptr[0] = loaded - base;
    phi->sources().ptr[0] = wholeBlock - base;
    phi->used().ptr[1] = partial - base;
    phi->sources().ptr[1] = join - base;
    after->addInst(base, (LowerInst*)phi);

    replaceAllUses(base, &((LowerInstVecLoadUpTo*)inst)->result, &phi->result);
}

} // namespace

void lowerMaskedTransfers(Context& ctx, LowerBase base, LowerFunction& fun) {
    // By index, as `expandAtomics` walks: the ladder appends blocks to the list being walked, and
    // moves the rest of the block it splits into one of them - which is where the walk finds it.
    for(Size b = 0; b < fun.blocks.size(); b++) {
        auto block = base[fun.blocks.get(base, b)];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::VecLoadUpTo && inst->kind != LowerInst::VecStoreUpTo) continue;

            Transfer transfer;
            Expansion e { base, fun, block, i };
            LowerValue* written;

            if(inst->kind == LowerInst::VecLoadUpTo) {
                auto load = (LowerInstVecLoadUpTo*)inst;
                transfer = { false, load->result.type, base[load->from], nullptr, nullptr };
                written = base[load->count];
            } else {
                auto store = (LowerInstVecStoreUpTo*)inst;
                transfer = { true, base[store->value]->type, base[store->to], nullptr, base[store->value] };
                written = base[store->count];
            }

            auto constant = written->inst()->kind == LowerInst::Imm;

            auto type = transfer.type;
            auto lanes = I64(type.lanes());
            auto width = type.byteWidth();

            /*
             * A constant count, which is decided here rather than tested: nothing at or below zero,
             * the whole vector at or above the lanes, and what is between left to the route - where a
             * constant makes the mask a constant as well.
             */
            if(constant) {
                auto count = constantCount(written);

                if(count <= 0 || count >= lanes) {
                    if(transfer.store) {
                        if(count > 0) e.store(transfer.address, transfer.value, width);
                    } else {
                        auto answer = count > 0 ? e.load(type, transfer.address, width)
                                                : e.reinterpret(type, e.splat(vectorType(LowerLane::Int8, width),
                                                                              e.integer(LowerType::Int32, 0)));
                        replaceAllUses(base, &((LowerInstVecLoadUpTo*)inst)->result, answer);
                    }

                    removeInst(base, inst);
                    i = e.at - 1;
                    continue;
                }
            }

            // A narrow constant is written again at sixty-four bits rather than converted, so that the
            // mask register's route still sees a constant and folds it.
            auto narrowConstant = constant && written->type != LowerType::Int64;
            transfer.count = narrowConstant ? e.integer(LowerType::Int64, U64(constantCount(written)))
                                            : countAsInt64(e, written);

            auto route = routeFor(type);
            switch(route) {
                case Route::MaskRegister:
                case Route::MaskVector: {
                    auto mask = route == Route::MaskRegister ? maskRegisterUpTo(e, transfer.count, type)
                                                             : maskVectorUpTo(ctx, e, transfer.count, type);

                    if(transfer.store) {
                        insertInstAt(base, block, e.at++, new (fun.arena) LowerInstX86MaskedStore(
                            transfer.address - base, mask - base, transfer.value - base));
                    } else {
                        auto loaded = e.emit(new (fun.arena) LowerInstX86MaskedLoad(
                            StringId(), type, transfer.address - base, mask - base));
                        replaceAllUses(base, &((LowerInstVecLoadUpTo*)inst)->result, loaded);
                    }

                    removeInst(base, inst);
                    i = e.at - 1;
                    break;
                }

                case Route::Ladder:
                    // The rest of this block has moved into one the walk reaches later.
                    rewriteByLadder(base, fun, block, e.at, inst, transfer);
                    i = block->instructions.size();
                    break;
            }
        }
    }
}
