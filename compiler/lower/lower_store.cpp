#include "lower_store.h"
#include "lower_fold.h"
#include "lower_builder.h"
#include "lower_promote.h"
#include "fold_arith.h"

namespace {

/*
 * How many writes the walk carries at once - see the record table in `forwardStoredValues`.
 *
 * It was one until J49, and eight until a record's fields stopped aliasing one another: while every
 * store to a record forgot the others, eight distinct allocations was more than a block had in
 * flight. Now the fields of one record are records of their own, and the run this has to hold is a
 * record zeroed field by field. The scan is linear in it and runs per access, so it is a constant
 * rather than a growable list on purpose.
 */
constexpr Size kRecords = 64;

// The mask is fold_arith.h's and the width lower_promote.h's `registerBits` - J163; this file had a
// copy of each (`maskOfBits`, and `typeBits`, which is `registerBits` for the integers it is asked of).
using foldArith::maskOf;

/*
 * The value a load of this store's pointer answers, or nothing.
 *
 * Three things have to hold, and each is a way the two can name different bits of one word:
 *
 *  - the same pointer - the same *value*, or two `add %p, k` chains down to the same root at the same
 *    offset (J168): `self.a = ..; self.b = ..` in two arms of an `if` that rejoin compute `self + 24`
 *    once per arm and once more below the join, and the lower tier's CSE declines to unify them
 *    across a call, so the three read-modify-writes of one packed word were three round trips
 *    through memory in the lexer's `advance`. Two addresses that walk to one root and one offset are
 *    one address whatever the root is, so this still needs no aliasing;
 *  - the same access width, since a narrower store under a wider load leaves the top of the answer
 *    to whatever was already in memory;
 *  - the same result type, and the load's extension already done - a four-byte load into a `Long`
 *    zeroes bits 32 to 63, so it is the stored value only where that value has them zero.
 */
LowerValue* forwardedValue(LowerBase base, LowerInstStore* store, LowerInstLoad* load) {
    if(store->isVolatile() || load->isVolatile()) return nullptr;
    if(store->getWidth() != load->getWidth()) return nullptr;

    if(store->to != load->from) {
        auto written = placeOf(base, store->to, store->getWidth());
        auto read = placeOf(base, load->from, load->getWidth());
        if(!written.root || written.root != read.root || written.offset != read.offset) return nullptr;
    }

    auto stored = base[store->value];
    if(stored->type != load->result.type) return nullptr;

    auto accessed = load->getWidth() * 8;
    auto bits = registerBits(load->result.type);
    if(accessed >= bits) return stored;

    // The load extends and the store did not, so the answer is the stored value only where the bits
    // above the access are already what the extension would put there. A signed load would need them
    // to be copies of a sign bit, which nothing here can say - only the zeroes are asked for.
    if(load->isSigned()) return nullptr;
    return (knownZeroBits(base, stored) & ~maskOf(accessed)) == ~maskOf(accessed)
        ? stored : nullptr;
}

// A second load of the word an earlier one read, answered by the first: the same width, into the
// same type, extended the same way - see the load records in `StoreForwarder::walk`.
LowerValue* repeatedValue(LowerBase base, LowerInstLoad* first, LowerInstLoad* load) {
    if(load->isVolatile() || load->isOverread()) return nullptr;
    if(first->getWidth() != load->getWidth() || first->isSigned() != load->isSigned()) return nullptr;
    if(first->result.type != load->result.type) return nullptr;

    if(first->from != load->from) {
        auto earlier = placeOf(base, first->from, first->getWidth());
        auto read = placeOf(base, load->from, load->getWidth());
        if(!earlier.root || earlier.root != read.root || earlier.offset != read.offset) return nullptr;
    }

    return &first->result;
}

/*
 * The low bytes of what a store wrote, read back at another width or into the other integer register
 * - J152. `forwardedValue` answers only a load that reads exactly what was written, in the register
 * it was written from; this answers the two shapes beside it that the lowering produces in the same
 * block:
 *
 *  - **a narrower load** of the pointer a store wrote - a sum's error byte read out of the word its
 *    value arm was stored as, or a tag read out of a word stored whole;
 *  - **the other register** at the same width - a four-byte field stored from an `I32` and read into
 *    an `I64`, which is what a `@bits` record's word is when a packed field is read out of it.
 *
 * Both are the stored value masked to the width the load reads and moved into the load's register,
 * extended the way the load extends. A constant is answered with a constant; anything else with the
 * `and` and the `cast` that say the same, placed where the load was. Integers only.
 */
LowerValue* reshapedValue(LowerBase base, LowerModule& module, LowerFunction& fun,
                          LowerPtr<LowerBlock> blockPtr, LowerInst* at, LowerInstStore* store,
                          LowerInstLoad* load, SmallArray<LowerPtr<LowerInst>, 32>& kept) {
    if(store->isVolatile() || load->isVolatile() || load->isOverread()) return nullptr;
    if(store->to != load->from || load->getWidth() > store->getWidth()) return nullptr;

    auto stored = base[store->value];
    auto type = load->result.type;

    auto emit = [&](LowerInst* made) {
        made->block = blockPtr;
        made->source = at->source;
        for(auto use: made->used()) base[use]->uses.push(fun.arena, made - base);
        kept.push(made - base);
        return &((LowerInstSingle*)made)->result;
    };

    // One register's bits read back as another's, whole - an address stored and read as the integer
    // of its width, which is how the aggregate convention carries a pointer field, or a float's bits.
    if(!isInt(stored->type) || !isInt(type)) {
        if(isVectorLike(stored->type) || isVectorLike(type) || stored->type == type) return nullptr;
        if(load->getWidth() != store->getWidth()) return nullptr;
        if(load->getWidth() * 8 != registerBits(type) || store->getWidth() * 8 != registerBits(stored->type)) {
            return nullptr;
        }

        return emit(new (module.arena) LowerInstUnary(LowerInst::Bitcast, StringId(), type, stored - base));
    }

    auto accessed = load->getWidth() * 8;
    auto bits = registerBits(type);
    auto signedLoad = load->isSigned() && accessed < bits;

    if(auto constant = stored->inst(); constant && constant->kind == LowerInst::Imm) {
        auto value = ((LowerImm*)constant)->i & maskOf(accessed);
        if(signedLoad && (value >> (accessed - 1)) & 1) value |= ~maskOf(accessed);

        return emit(new (module.arena) LowerImm(StringId(), type, value & maskOf(bits)));
    }

    auto value = stored;

    // The bytes the load reads, and nothing above them - unless nothing above them is set anyway.
    if(accessed < registerBits(stored->type) &&
       (knownZeroBits(base, stored) & ~maskOf(accessed)) != (~maskOf(accessed) & maskOf(registerBits(stored->type)))) {
        auto mask = emit(new (module.arena) LowerImm(StringId(), stored->type, maskOf(accessed)));
        value = emit(new (module.arena) LowerInstBinary(StringId(), stored->type, value - base, mask - base,
                                                        LowerInst::And));
    }

    if(value->type != type) {
        value = emit(new (module.arena) LowerInstCast(StringId(), type, value - base, false, false));
    }

    if(signedLoad) {
        auto shift = emit(new (module.arena) LowerImm(StringId(), type, U64(bits - accessed)));
        auto up = emit(new (module.arena) LowerInstBinary(StringId(), type, value - base, shift - base,
                                                          LowerInst::Shl));
        value = emit(new (module.arena) LowerInstBinary(StringId(), type, up - base, shift - base,
                                                        LowerInst::Sar));
    }

    return value == stored ? nullptr : value;
}

} // namespace

/*
 * Whether every use of an address, through constant offsets, is the address of an access.
 *
 * Such an allocation is reachable through no pointer but its own: nothing stored its address, passed
 * it, merged it in a phi or indexed it at run time. So a place under any *other* root - a parameter,
 * a call's result, a load - is not one of its bytes, which is the fact a slot needs to be told apart
 * from the pointer a copy into it reads. `let copy = token` is that copy, and without this the fill
 * zeroing the slot first was observed by it.
 *
 * The depth is `placeOf`'s: an address this walk accepts is one that walk reaches the root of.
 */
bool addressContained(LowerBase base, LowerValue* address, U32 depth) {
    if(depth >= 8) return false;
    auto self = address - base;

    for(auto userPtr: address->uses.contents(base)) {
        auto user = base[userPtr];

        switch(user->kind) {
            case LowerInst::Load:
                break;
            case LowerInst::Store:
                if(((LowerInstStore*)user)->value == self) return false;
                break;
            case LowerInst::Copy:
                if(((LowerInstCopy*)user)->count == self) return false;
                break;
            case LowerInst::SetPattern: {
                auto fill = (LowerInstSetPattern*)user;
                if(fill->count == self || fill->pattern == self) return false;
                break;
            }
            case LowerInst::Add: {
                auto add = (LowerInstBinary*)user;
                auto other = base[add->lhs == self ? add->rhs : add->lhs]->inst();
                if(add->lhs == add->rhs || !other || other->kind != LowerInst::Imm) return false;
                if(((LowerImm*)other)->i > (U64(1) << 32)) return false;
                if(!addressContained(base, &add->result, depth + 1)) return false;
                break;
            }
            default:
                return false;
        }
    }

    return true;
}

namespace {

LowerPlace walkPlace(LowerBase base, HashMap<U32, U8>* contained, LowerPtr<LowerValue> pointer, U64 bytes) {
    auto value = base[pointer];
    U64 offset = 0;

    for(auto depth = 0; ; depth++) {
        auto inst = value->inst();
        if(!inst || inst->kind != LowerInst::Add) break;

        auto add = (LowerInstBinary*)inst;
        auto lhs = base[add->lhs]->inst();
        auto rhs = base[add->rhs]->inst();

        LowerImm* amount;
        if(rhs && rhs->kind == LowerInst::Imm) { amount = (LowerImm*)rhs; value = base[add->lhs]; }
        else if(lhs && lhs->kind == LowerInst::Imm) { amount = (LowerImm*)lhs; value = base[add->rhs]; }
        else break;

        // An offset this far from its root is a negative one, or one about to wrap - neither is a
        // field, and a range compared across the wrap would be a wrong answer rather than a cautious
        // one. Nor is a chain deeper than `addressContained` follows one it may be compared under:
        // a pointer this far from its root is not a shape lowering emits, and a budget is what keeps
        // one question from walking a dataflow graph.
        offset += amount->i;
        if(depth == 8 || amount->i > (U64(1) << 32) || offset > (U64(1) << 32)) return LowerPlace { .bytes = bytes };
    }

    auto inst = value->inst();
    auto allocation = inst && inst->kind == LowerInst::Alloca;

    return LowerPlace {
        .root = value,
        .allocation = allocation,
        .contained = allocation && contained && contained->getValue(U32(value - base)).isJust(),
        .offset = offset,
        .bytes = bytes,
    };
}

} // namespace

LowerPlace placeOf(LowerBase base, HashMap<U32, U8>& contained, LowerPtr<LowerValue> pointer, U64 bytes) {
    return walkPlace(base, &contained, pointer, bytes);
}

LowerPlace placeOf(LowerBase base, LowerPtr<LowerValue> pointer, U64 bytes) {
    return walkPlace(base, nullptr, pointer, bytes);
}

// The extent an access states, where it states one.
U64 placeExtent(LowerBase base, const LowerAccess& access) {
    if(access.bytes) return access.bytes;
    if(!access.count) return kUnbounded;

    auto count = base[access.count]->inst();
    return count && count->kind == LowerInst::Imm ? ((LowerImm*)count)->i : kUnbounded;
}

U64 placeEnd(const LowerPlace& place) {
    return place.bytes == kUnbounded ? kUnbounded : place.offset + place.bytes;
}

/*
 * Whether two places may share a byte - the one aliasing question this pass asks.
 *
 * **Two different `Alloca`s are two different objects**, and under one root the ranges decide. A
 * contained allocation shares no byte with a place under any other root. Anything else may be
 * anything at all: a parameter can point into a record whose address escaped, so a pair under two
 * roots that are neither is reported as possibly overlapping - and so is a place with no root.
 */
bool mayOverlap(const LowerPlace& a, const LowerPlace& b) {
    if(!a.root || !b.root) return true;
    if(a.root != b.root) return !(a.allocation && b.allocation) && !a.contained && !b.contained;
    if(a.bytes == 0 || b.bytes == 0) return false;
    return a.offset < placeEnd(b) && b.offset < placeEnd(a);
}

namespace {

// Whether a write to `outer` replaces every byte of `inner`.
bool covers(const LowerPlace& outer, const LowerPlace& inner) {
    if(!outer.root || outer.root != inner.root || inner.bytes == kUnbounded || outer.bytes == kUnbounded) return false;
    return outer.offset <= inner.offset && placeEnd(inner) <= placeEnd(outer);
}

/*
 * Whether a plain write puts nothing but zero bytes where it writes - a fill of a zero byte, or a store
 * of a zero literal. A floating literal holds a `double` whatever its type, so its whole pattern is
 * what says zero; an integer's bits above the access are not written.
 */
bool writesZero(LowerBase base, LowerInst* inst) {
    auto literal = [&](LowerPtr<LowerValue> value, U64 mask) {
        auto source = base[value]->inst();
        if(source && source->kind == LowerInst::Bitcast) source = base[((LowerInstUnary*)source)->from]->inst();
        if(!source || source->kind != LowerInst::Imm) return false;

        auto imm = (LowerImm*)source;
        return (isFloat(imm->result.type) ? imm->i : imm->i & mask) == 0;
    };

    if(inst->kind == LowerInst::SetPattern) return literal(((LowerInstSetPattern*)inst)->pattern, 0xff);
    if(inst->kind != LowerInst::Store) return false;

    auto store = (LowerInstStore*)inst;
    return literal(store->value, maskOf(store->getWidth() * 8));
}

} // namespace

void collectContainedAllocations(LowerBase base, LowerFunction& fun, HashMap<U32, U8>& into) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
            auto inst = base[instPtr];
            if(inst->kind != LowerInst::Alloca) continue;

            auto address = &((LowerInstSingle*)inst)->result;
            if(addressContained(base, address, 0)) into.add(U32(address - base), 1);
        }
    }
}

namespace {

/*
 * A write the walk is still inside the answer to, where it landed, and whether anything since could
 * have read what it wrote. The last is what decides whether a later write that replaces all of its
 * bytes makes it dead - `%c` becoming `%b` is what makes the store that produced it one.
 */
struct Record {
    LowerInst* write = nullptr;
    LowerPlace place;
    bool observed = false;

    // Whether a call has run since - see the reshaped forwarding below.
    bool crossedCall = false;
};

struct StoreForwarder {
    LowerBase base;
    LowerModule& module;
    LowerFunction& fun;

    // The allocations whose address never leaves, asked once per function - see `addressContained`.
    HashMap<U32, U8> contained;

    // What stands at the end of each block, indexed by `LowerBlock::index`: a span of `ends`.
    Array<Record> ends;
    Array<Size> endStart;
    Array<Size> endCount;
    Array<bool> done;

    StoreForwarder(LowerBase base, LowerModule& module, LowerFunction& fun):
        base(base), module(module), fun(fun) {}

    /*
     * What stands on entry to a block: everything that stood at the end of its one predecessor - J169.
     *
     * The walk used to start every block empty, so a word stored above a branch and read in the arm
     * below it was a round trip through memory, and so was every value a block handed on through the
     * frame to the block it jumps to. A block with one predecessor is dominated by it, so every value
     * a record there names is defined here too, and nothing ran on the way in.
     *
     * Observed, because the predecessor's other successor may read the word: no write here can make
     * a store above the edge dead.
     *
     * **Not at a join, and that was built and measured out.** A word every predecessor stores - the
     * lexer's `advance` writes one byte in four arms of an `if` and reads it again below - was
     * answered with a phi of the stored values, built by the first load that asked, with a load moved
     * to the end of an edge that stored nothing but only jumped there (the partial redundancy
     * `recoverPartialLoads` removes for a guard's reads). It removed the load and moved nothing: the
     * lexer was within its control copy on three variants and `hand` 1% slower, `test/base` 64 bytes
     * either way, and `parseSymbol`'s chain of arms storing a token kind became a 32-way phi of
     * literals - a move per edge where there had been one load, 184 bytes. Threading the compare
     * that phi feeds would have paid, and needs the SSA reconstruction lower_thread.h stops short of.
     * Nor across a back edge, where what the latch leaves is not known yet; `promoteLoopCells` is
     * what carries a word around a loop.
     */
    Size entryOf(LowerBlock* block, Record* records) {
        if(block->incoming.size() != 1) return 0;

        auto from = base[block->incoming.get(base, 0)];
        if(from->postIndex == kNullBlock || from->postIndex <= block->postIndex) return 0;

        auto count = endCount[from->index];
        for(Size i = 0; i < count; i++) {
            records[i] = ends[endStart[from->index] + i];
            records[i].observed = true;
        }

        return count;
    }

    void walk(LowerPtr<LowerBlock> blockPtr);
};

} // namespace

void forwardStoredValues(LowerBase base, LowerModule& module, LowerFunction& fun) {
    StoreForwarder forwarder { base, module, fun };
    collectContainedAllocations(base, fun, forwarder.contained);

    // In reverse postorder, so that a block's predecessor has been walked before it. Renumbers
    // `index` as well, which every pass keeps equal to the block's position anyway.
    auto postorder = fun.buildPostorder(base);
    auto blockCount = fun.blocks.size();

    for(Size i = 0; i < blockCount; i++) {
        forwarder.endStart.push(0);
        forwarder.endCount.push(0);
        forwarder.done.push(false);
    }

    for(auto i = postorder.size(); i > 0; i--) forwarder.walk(fun.blocks.get(base, postorder[i - 1]));

    // A block nothing reaches still has accesses of its own.
    for(Size i = 0; i < blockCount; i++) {
        if(!forwarder.done[i]) forwarder.walk(fun.blocks.get(base, i));
    }
}

void StoreForwarder::walk(LowerPtr<LowerBlock> blockPtr) {
    auto block = base[blockPtr];

    // Inline: one of these per block, holding the instructions of that block while the list it came
    // from is rewritten - the same shape as foldFunctionConstants, and for the same reason.
    SmallArray<LowerPtr<LowerInst>, 32> kept;
    auto rewrote = false;

    /*
     * **Several rather than one** - J49, `test/bench/findings.md` §99 - and since then a copy and a
     * fill as well as a store. A record zeroed field by field and then overwritten whole by a `copy`
     * is how a value built in a frame slot and then replaced reaches this tier, and the zeroing was a
     * run of stores nothing read.
     *
     * Bounded: the cost is a linear scan per access over a list this long, and the oldest record is
     * dropped when it is full. Sixty-four because the run above is one record's fields, and a record
     * of forty fields is not rare in a compiler's own data.
     */
    Record records[kRecords];
    Size recordCount = block->postIndex == kNullBlock ? 0 : entryOf(block, records);

    auto drop = [&](Size index) {
        for(auto i = index + 1; i < recordCount; i++) records[i - 1] = records[i];
        recordCount--;
    };

    for(auto instPtr: block->instructions.contents(base)) {
        auto inst = base[instPtr];

        if(inst->kind == LowerInst::Load) {
            auto load = (LowerInstLoad*)inst;
            LowerValue* forwarded = nullptr;

            // Newest first: where two records answer, the later store is the one in the word.
            for(auto i = recordCount; i > 0 && !forwarded; i--) {
                if(records[i - 1].write->kind == LowerInst::Load) {
                    if(!records[i - 1].crossedCall) forwarded = repeatedValue(base, (LowerInstLoad*)records[i - 1].write, load);
                    continue;
                }

                if(records[i - 1].write->kind != LowerInst::Store) continue;
                forwarded = forwardedValue(base, (LowerInstStore*)records[i - 1].write, load);
            }

            /*
             * Or its low bytes, in the load's register - see reshapedValue. Not across a call: the
             * store stays for whatever else reads the slot, so the value would be held in a register
             * over the call as well as in memory, and the reload was the cheaper of the two -
             * measured on `test/base`, where the view words re-read after a second call were half a
             * kilobyte of spills.
             */
            for(auto i = recordCount; i > 0 && !forwarded; i--) {
                if(records[i - 1].write->kind != LowerInst::Store || records[i - 1].crossedCall) continue;
                forwarded = reshapedValue(base, module, fun, blockPtr, inst,
                                          (LowerInstStore*)records[i - 1].write, load, kept);
            }

            /*
             * Or a zero, where every byte the load reads was last written zero - a fill, or a zero
             * store wider than the load. A record still standing is the last write to its bytes,
             * since any later write that touched them dropped it.
             *
             * This is what a zeroed record's first read-modify-write of a packed word needs: it used
             * to read back the zero store `zeroStorage` wrote, through the identity above, and a fill
             * is no store. Only an integer, which is what a packed word is read as and the one
             * register whose zero is a literal of its own type.
             */
            auto type = load->result.type;
            if(!load->isVolatile() && !forwarded && isInt(type) && !load->isOverread()) {
                auto place = placeOf(base, contained, load->from, load->getWidth());

                for(Size i = 0; i < recordCount; i++) {
                    if(!covers(records[i].place, place) || !writesZero(base, records[i].write)) continue;

                    auto zero = new (module.arena) LowerImm(StringId(), type, U64(0));
                    zero->block = blockPtr;
                    zero->source = inst->source;
                    kept.push((LowerInst*)zero - base);
                    forwarded = &zero->result;
                    break;
                }
            }

            if(forwarded) {
                /*
                 * A literal stored in a block above gets one of its own here, for the reason
                 * `recoverPartialLoads` gives its moved instructions one: a literal read from two
                 * blocks is what the loop hoister lifts into a preheader and the allocator then
                 * spills, where one placed at its reader is free.
                 */
                if(auto constant = forwarded->inst(); constant->kind == LowerInst::Imm && constant->block != blockPtr) {
                    auto copy = new (module.arena) LowerImm(StringId(), forwarded->type, ((LowerImm*)constant)->i);
                    copy->block = blockPtr;
                    copy->source = inst->source;
                    kept.push((LowerInst*)copy - base);
                    forwarded = &copy->result;
                }

                detach(base, inst);
                replaceUses(base, module.arena, load->created().ptr - base, forwarded - base);
                rewrote = true;
                continue;
            }
        }

        LowerAccess accesses[kMaxAccesses];
        auto accessCount = lowerInstAccesses(inst, accesses);
        auto reads = hasLowerTrait(inst, kLowerReads);
        auto writes = hasLowerTrait(inst, kLowerWrites);

        /*
         * Storage touched through no address - a call, an intrinsic, a fence. A write may have
         * written any record's bytes, which ends the record outright; a read may have read them,
         * which leaves them forwardable and only says they were observed.
         *
         * Asked through the traits rather than by kind, which is what brings the atomics into a
         * pass that could not see them at all before: an `atomic_rmw` between two stores of one
         * pointer used to be invisible here, so the first store was deleted as dead although the
         * update had read it. Reading it as a *read* rather than as a barrier is deliberate -
         * ordering is what an atomic does for other threads, and this pass reasons about a plain
         * location no other thread may legally touch, so what matters is only whether the bytes may
         * have moved.
         */
        if(accessCount == 0) {
            // Except a contained allocation, which nothing reaches but its own address - and an
            // instruction naming no address has not been given it.
            for(auto i = recordCount; i > 0; i--) {
                if(inst->kind == LowerInst::Call) records[i - 1].crossedCall = true;
                if(records[i - 1].place.contained) continue;
                if(writes) drop(i - 1);
                else if(reads) records[i - 1].observed = true;
            }

            kept.push(instPtr);
            continue;
        }

        // A copy reads its second access and writes its first. Every other kind naming an address
        // does to each of them what its traits say.
        auto isCopy = inst->kind == LowerInst::Copy;
        LowerPlace written[kMaxAccesses];
        Size writtenCount = 0;

        for(Size a = 0; a < accessCount; a++) {
            auto bytes = placeExtent(base, accesses[a]);
            auto accessReads = isCopy ? a == 1 : reads;
            auto accessWrites = isCopy ? a == 0 : writes;

            // A load reading past its end on the tail-read guarantee reads bytes it does not name,
            // which the place has to include - see isOverreadLoad.
            if(inst->kind == LowerInst::Load && ((LowerInstLoad*)inst)->isOverread()) bytes = kUnbounded;

            auto place = placeOf(base, contained, *accesses[a].address, bytes);

            if(accessReads) {
                for(Size i = 0; i < recordCount; i++) {
                    if(mayOverlap(records[i].place, place)) records[i].observed = true;
                }
            }

            if(accessWrites) written[writtenCount++] = place;
        }

        // Reads first, then writes: a copy whose source is the record it replaces has observed it.
        // Only a plain write replaces what it covers - an atomic write is an ordering, and what it
        // overwrote is something another thread may have been owed.
        auto plainWrite = inst->kind == LowerInst::Store || isCopy || inst->kind == LowerInst::SetPattern;

        /*
         * A zero written where an earlier zero write still stands is no write at all - a record
         * literal's zero fields in a slot `hasPaddedWord` already cleared, or a field stored zero
         * behind the fill that cleared it. Observed or not makes no difference: whatever read the
         * earlier one read the same zeroes. A record still standing is one no write since has
         * touched, which is what `drop` below keeps true - and one from the block above stands on
         * the only way here.
         */
        if(plainWrite && writtenCount == 1 && writesZero(base, inst)) {
            auto redundant = false;

            for(Size i = 0; i < recordCount && !redundant; i++) {
                redundant = covers(records[i].place, written[0]) && writesZero(base, records[i].write);
            }

            if(redundant) {
                detach(base, inst);
                rewrote = true;
                continue;
            }
        }

        for(Size w = 0; w < writtenCount; w++) {
            auto& place = written[w];

            for(auto i = recordCount; i > 0; i--) {
                auto& record = records[i - 1];

                // Never a record from above this block, which is observed: the other way out of the
                // block that wrote it may read it.
                if(plainWrite && !record.observed && covers(place, record.place)) {
                    detach(base, record.write);
                    kept.removeFirst(record.write - base);
                    rewrote = true;
                    drop(i - 1);
                    continue;
                }

                // A record this write may have changed a byte of is no longer the answer to a load,
                // and is not one a later write can be shown to replace either.
                if(mayOverlap(place, record.place)) drop(i - 1);
            }

            if(plainWrite && place.bytes != kUnbounded && place.bytes != 0) {
                if(recordCount == kRecords) drop(0);
                records[recordCount].write = inst;
                records[recordCount].place = place;
                records[recordCount].observed = false;
                records[recordCount].crossedCall = false;
                recordCount++;
            }
        }

        /*
         * A load is a record too, of what the word held when it read it - J180. A second read of the
         * same word with nothing between that may have written it is the first read's value: the
         * lexer's `skipWhitespace` tests `self.p < self.m` at the top of its loop and reads `self.p`
         * again for the character at the top of the body, one block down. The same invalidation
         * applies as to a store's record, since a record is dropped by any write that may overlap
         * it; it is observed from the start, so no write ever takes it for a dead store. Not across
         * a call, for the reason the reshaping gives: holding the value over the call costs a
         * callee-saved register, and the reload was the cheaper of the two.
         */
        if(inst->kind == LowerInst::Load && !((LowerInstLoad*)inst)->isVolatile() &&
           !((LowerInstLoad*)inst)->isOverread())
        {
            auto load = (LowerInstLoad*)inst;
            auto place = placeOf(base, contained, load->from, load->getWidth());

            if(place.root && place.bytes != 0) {
                if(recordCount == kRecords) drop(0);
                records[recordCount].write = inst;
                records[recordCount].place = place;
                records[recordCount].observed = true;
                records[recordCount].crossedCall = false;
                recordCount++;
            }
        }

        kept.push(instPtr);
    }

    endStart[block->index] = ends.size();
    endCount[block->index] = recordCount;
    done[block->index] = true;
    for(Size i = 0; i < recordCount; i++) ends.push(records[i]);

    if(!rewrote) return;

    block->instructions.clear();
    for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
}
