#include "lower_store.h"
#include "lower_fold.h"
#include "lower_builder.h"

namespace {

/*
 * How many stores the walk carries at once - see the record table in `forwardStoredValues`.
 *
 * It was one until J49. Eight because the shape this exists for is a handful of frame slots written
 * in one block and read back below, and a block with more distinct allocations than this in flight
 * is one whose oldest record is not what the next load will ask for. The scan is linear in it and
 * runs per store, so it is a constant rather than a growable list on purpose.
 */
constexpr Size kRecords = 8;

U64 maskOfBits(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

U32 typeBits(LowerType type) {
    return type == LowerType::Int32 ? 32 : 64;
}

/*
 * The value a load of this store's pointer answers, or nothing.
 *
 * Three things have to hold, and each is a way the two can name different bits of one word:
 *
 *  - the same pointer *value*, which is what makes this need no aliasing;
 *  - the same access width, since a narrower store under a wider load leaves the top of the answer
 *    to whatever was already in memory;
 *  - the same result type, and the load's extension already done - a four-byte load into a `Long`
 *    zeroes bits 32 to 63, so it is the stored value only where that value has them zero.
 */
LowerValue* forwardedValue(LowerBase base, LowerInstStore* store, LowerInstLoad* load) {
    if(store->to != load->from) return nullptr;
    if(store->getWidth() != load->getWidth()) return nullptr;

    auto stored = base[store->value];
    if(stored->type != load->result.type) return nullptr;

    auto accessed = load->getWidth() * 8;
    auto bits = typeBits(load->result.type);
    if(accessed >= bits) return stored;

    // The load extends and the store did not, so the answer is the stored value only where the bits
    // above the access are already what the extension would put there. A signed load would need them
    // to be copies of a sign bit, which nothing here can say - only the zeroes are asked for.
    if(load->isSigned()) return nullptr;
    return (knownZeroBits(base, stored) & ~maskOfBits(accessed)) == ~maskOfBits(accessed)
        ? stored : nullptr;
}

/*
 * The allocation a pointer names, walked through the constant offsets that name a field of it.
 *
 * `add %slot, 8` is how every field past the first is addressed, so a walk that stopped at the
 * `Alloca` itself would answer for a record's first field and for nothing else. Only `Add` and only
 * where the other operand is a literal: an offset this pass cannot read is one that could have left
 * the allocation, and the answer then is "no allocation" rather than a guess.
 *
 * Null where the chain reaches anything else - a parameter, a call's result, a global. That is the
 * conservative answer, and `distinctStorage` below is written so that it stays one.
 */
LowerInst* storageOf(LowerBase base, LowerPtr<LowerValue> pointer) {
    auto value = base[pointer];

    for(auto depth = 0; depth < 8; depth++) {
        auto inst = value->inst();
        if(!inst) return nullptr;
        if(inst->kind == LowerInst::Alloca) return inst;
        if(inst->kind != LowerInst::Add) return nullptr;

        auto add = (LowerInstBinary*)inst;
        auto lhs = base[add->lhs]->inst();
        auto rhs = base[add->rhs]->inst();
        if(rhs && rhs->kind == LowerInst::Imm) value = base[add->lhs];
        else if(lhs && lhs->kind == LowerInst::Imm) value = base[add->rhs];
        else return nullptr;
    }

    return nullptr;
}

/*
 * Whether two pointers name storage that cannot overlap - the one aliasing fact this pass is willing
 * to establish, and it is an identity rather than a range test.
 *
 * **Two different `Alloca`s are two different objects.** Nothing else here is: a pointer whose
 * allocation this cannot name may be anything at all, so a pair with a null on either side is
 * reported as possibly overlapping and the record is dropped. Within one allocation the offsets are
 * *not* compared, which would be the range test this deliberately is not - two fields of one record
 * simply alias as far as this pass is concerned, which is what the single-record version assumed
 * about every pair.
 *
 * That narrow fact is enough for the shape this exists for: a record built in a frame slot while
 * another frame slot holds a word that is read back below it. `test/bench/findings.md` §99.
 */
bool distinctStorage(LowerBase base, LowerPtr<LowerValue> a, LowerPtr<LowerValue> b) {
    auto left = storageOf(base, a);
    auto right = storageOf(base, b);
    return left && right && left != right;
}

} // namespace

void forwardStoredValues(LowerBase base, LowerModule& module, LowerFunction& fun) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        // Inline: one of these per block, holding the instructions of that block while the list it
        // came from is rewritten - the same shape as foldFunctionConstants, and for the same reason.
        SmallArray<LowerPtr<LowerInst>, 32> kept;
        auto rewrote = false;

        /*
         * The stores the walk is still inside the answer to, and per store whether anything since it
         * could have read what it wrote. The second is what decides whether a store to the same
         * pointer overwrites a word nothing observed, which is the other half of the pair - `%c`
         * becoming `%b` is what makes the store that produced it dead.
         *
         * **Several rather than one** - J49, `test/bench/findings.md` §99. It was a single record,
         * on the argument that a second store clears the first whether or not the two addresses have
         * anything to do with each other; that is true of two addresses this pass cannot tell apart
         * and false of two different `Alloca`s, which is the pair that turns up the moment a record
         * is built in one frame slot while a word read back below it sits in another. Placing a run
         * on the frame made exactly that shape common, and the word left behind was a run's packed
         * capacity written, four element stores emitted between, and then loaded back.
         *
         * Bounded, and small: the cost of the table is a linear scan per store over a list that is
         * this long, and a block writing more than a handful of distinct allocations is one where
         * the oldest record is not what the next load is going to ask for. The oldest is dropped.
         */
        struct Record {
            LowerInstStore* store = nullptr;
            bool observed = false;
        };

        Record records[kRecords];
        Size recordCount = 0;

        auto drop = [&](Size index) {
            for(auto i = index + 1; i < recordCount; i++) records[i - 1] = records[i];
            recordCount--;
        };

        // Every record this pointer may have been written over, dropped. Backwards, so that removing
        // one does not move a record the loop has yet to look at.
        auto forget = [&](LowerPtr<LowerValue> pointer) {
            for(auto i = recordCount; i > 0; i--) {
                if(!distinctStorage(base, records[i - 1].store->to, pointer)) drop(i - 1);
            }
        };

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            if(inst->kind == LowerInst::Load) {
                auto load = (LowerInstLoad*)inst;
                LowerValue* forwarded = nullptr;

                // Newest first: where two records answer, the later store is the one in the word.
                for(auto i = recordCount; i > 0 && !forwarded; i--) {
                    forwarded = forwardedValue(base, records[i - 1].store, load);
                }

                if(forwarded) {
                    detach(base, inst);
                    replaceUses(base, module.arena, load->created().ptr - base, forwarded - base);
                    rewrote = true;
                    continue;
                }
            }


            if(inst->kind == LowerInst::Store) {
                auto store = (LowerInstStore*)inst;

                /*
                 * A store whose word is written again before anything reads it. The pointer and the
                 * width have to be the same, which is the same identity test the forwarding above
                 * makes: a wider store covering a narrower one is a fact about addresses that this
                 * pass has no way to establish.
                 */
                for(Size i = 0; i < recordCount; i++) {
                    auto& record = records[i];
                    if(record.observed || record.store->to != store->to) continue;
                    if(record.store->getWidth() != store->getWidth()) continue;

                    detach(base, (LowerInst*)record.store);
                    kept.removeFirst((LowerInst*)record.store - base);
                    rewrote = true;
                    break;
                }

                // And every record this store may have written over, which is every one whose
                // storage is not provably another object.
                forget(store->to);

                if(recordCount == kRecords) drop(0);
                records[recordCount].store = store;
                records[recordCount].observed = false;
                recordCount++;
                kept.push(instPtr);
                continue;
            }

            /*
             * A write may have written this word, which ends the record outright; a read may have
             * read it, which leaves the record forwardable and only says the store is one something
             * has observed.
             *
             * Asked through the traits rather than by kind, which is what brings the atomics into a
             * pass that could not see them at all before: an `atomic_rmw` between two stores of one
             * pointer used to be invisible here, so the first store was deleted as dead although the
             * update had read it. Reading it as a *read* rather than as a barrier is deliberate -
             * ordering is what an atomic does for other threads, and this pass reasons about a plain
             * location no other thread may legally touch, so what matters is only whether the bytes
             * may have moved.
             */
            if(hasLowerTrait(inst, kLowerWrites)) recordCount = 0;
            else if(hasLowerTrait(inst, kLowerReads)) {
                for(Size i = 0; i < recordCount; i++) records[i].observed = true;
            }

            kept.push(instPtr);
        }

        if(!rewrote) continue;

        block->instructions.clear();
        for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
    }
}
