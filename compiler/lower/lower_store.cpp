#include "lower_store.h"
#include "lower_fold.h"
#include "lower_builder.h"

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
 * Where an access lands, as far as this pass can name it: a root value, a constant byte offset from
 * it, and how many bytes the access reaches.
 *
 * `add %slot, 8` is how every field past the first is addressed, so the root is found by walking
 * through the constant offsets that name a field. The walk stops at *any* value - an `Alloca`, a
 * parameter, a call's result - and what it reached is the root either way: two accesses under one
 * root value are the same object, and their offsets can be compared as numbers. Two under different
 * roots are only known apart where both roots are allocations.
 *
 * It used to be the allocation alone, and two fields of one record aliased as far as this pass was
 * concerned. That made every store to a record forget every other store to it, so a record zeroed
 * field by field and then written again - which is what a fresh value built in a frame slot is - kept
 * the whole first run.
 */
constexpr U64 kUnbounded = maxLimit<U64>;

struct Place {
    // Null where the walk could not finish - a place that may be anywhere.
    LowerValue* root = nullptr;
    bool allocation = false;

    // An allocation whose address is only ever used to reach its own bytes - see `addressContained`.
    bool contained = false;

    U64 offset = 0;
    U64 bytes = 0;
};

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

Place placeOf(LowerBase base, HashMap<U32, U8>& contained, LowerPtr<LowerValue> pointer, U64 bytes) {
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
        // one. Nor is a chain deeper than `addressContained` follows one it may be compared under.
        offset += amount->i;
        if(depth == 8 || amount->i > (U64(1) << 32) || offset > (U64(1) << 32)) return Place { .bytes = bytes };
    }

    auto inst = value->inst();
    auto allocation = inst && inst->kind == LowerInst::Alloca;

    return Place {
        .root = value,
        .allocation = allocation,
        .contained = allocation && contained.getValue(U32(value - base)).isJust(),
        .offset = offset,
        .bytes = bytes,
    };
}

// The extent an access states, where it states one.
U64 accessBytes(LowerBase base, const LowerAccess& access) {
    if(access.bytes) return access.bytes;
    if(!access.count) return kUnbounded;

    auto count = base[access.count]->inst();
    return count && count->kind == LowerInst::Imm ? ((LowerImm*)count)->i : kUnbounded;
}

U64 placeEnd(const Place& place) {
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
bool mayOverlap(const Place& a, const Place& b) {
    if(!a.root || !b.root) return true;
    if(a.root != b.root) return !(a.allocation && b.allocation) && !a.contained && !b.contained;
    if(a.bytes == 0 || b.bytes == 0) return false;
    return a.offset < placeEnd(b) && b.offset < placeEnd(a);
}

// Whether a write to `outer` replaces every byte of `inner`.
bool covers(const Place& outer, const Place& inner) {
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
    return literal(store->value, maskOfBits(store->getWidth() * 8));
}

} // namespace

void forwardStoredValues(LowerBase base, LowerModule& module, LowerFunction& fun) {
    // The allocations whose address never leaves, asked once per function - see `addressContained`.
    HashMap<U32, U8> contained;

    for(auto blockPtr: fun.blocks.contents(base)) {
        for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
            auto inst = base[instPtr];
            if(inst->kind != LowerInst::Alloca) continue;

            auto address = &((LowerInstSingle*)inst)->result;
            if(addressContained(base, address, 0)) contained.add(U32(address - base), 1);
        }
    }

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        // Inline: one of these per block, holding the instructions of that block while the list it
        // came from is rewritten - the same shape as foldFunctionConstants, and for the same reason.
        SmallArray<LowerPtr<LowerInst>, 32> kept;
        auto rewrote = false;

        /*
         * The writes the walk is still inside the answer to, where each landed, and whether anything
         * since could have read what it wrote. The last is what decides whether a later write that
         * replaces all of its bytes makes it dead - `%c` becoming `%b` is what makes the store that
         * produced it one.
         *
         * **Several rather than one** - J49, `test/bench/findings.md` §99 - and since then a copy
         * and a fill as well as a store. A record zeroed field by field and then overwritten whole
         * by a `copy` is how a value built in a frame slot and then replaced reaches this tier, and
         * the zeroing was a run of stores nothing read.
         *
         * Bounded: the cost is a linear scan per access over a list this long, and the oldest record
         * is dropped when it is full. Sixty-four because the run above is one record's fields, and a
         * record of forty fields is not rare in a compiler's own data.
         */
        struct Record {
            LowerInst* write = nullptr;
            Place place;
            bool observed = false;
        };

        Record records[kRecords];
        Size recordCount = 0;

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
                    if(records[i - 1].write->kind != LowerInst::Store) continue;
                    forwarded = forwardedValue(base, (LowerInstStore*)records[i - 1].write, load);
                }

                /*
                 * Or a zero, where every byte the load reads was last written zero - a fill, or a
                 * zero store wider than the load. A record still standing is the last write to its
                 * bytes, since any later write that touched them dropped it.
                 *
                 * This is what a zeroed record's first read-modify-write of a packed word needs: it
                 * used to read back the zero store `zeroStorage` wrote, through the identity above,
                 * and a fill is no store. Only an integer, which is what a packed word is read as and
                 * the one register whose zero is a literal of its own type.
                 */
                auto type = load->result.type;
                if(!forwarded && isInt(type) && !load->isOverread()) {
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
             * written any record's bytes, which ends the record outright; a read may have read
             * them, which leaves them forwardable and only says they were observed.
             *
             * Asked through the traits rather than by kind, which is what brings the atomics into a
             * pass that could not see them at all before: an `atomic_rmw` between two stores of one
             * pointer used to be invisible here, so the first store was deleted as dead although the
             * update had read it. Reading it as a *read* rather than as a barrier is deliberate -
             * ordering is what an atomic does for other threads, and this pass reasons about a plain
             * location no other thread may legally touch, so what matters is only whether the bytes
             * may have moved.
             */
            if(accessCount == 0) {
                // Except a contained allocation, which nothing reaches but its own address - and an
                // instruction naming no address has not been given it.
                for(auto i = recordCount; i > 0; i--) {
                    if(records[i - 1].place.contained) continue;
                    if(writes) drop(i - 1);
                    else if(reads) records[i - 1].observed = true;
                }

                kept.push(instPtr);
                continue;
            }

            // A copy reads its second access and writes its first. Every other kind naming an
            // address does to each of them what its traits say.
            auto isCopy = inst->kind == LowerInst::Copy;
            Place written[kMaxAccesses];
            Size writtenCount = 0;

            for(Size a = 0; a < accessCount; a++) {
                auto bytes = accessBytes(base, accesses[a]);
                auto accessReads = isCopy ? a == 1 : reads;
                auto accessWrites = isCopy ? a == 0 : writes;

                // A load reading past its end on the tail-read guarantee reads bytes it does not
                // name, which the place has to include - see isOverreadLoad.
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
            // Only a plain write replaces what it covers - an atomic write is an ordering, and what
            // it overwrote is something another thread may have been owed.
            auto plainWrite = inst->kind == LowerInst::Store || isCopy || inst->kind == LowerInst::SetPattern;

            /*
             * A zero written where an earlier zero write still stands is no write at all - a record
             * literal's zero fields in a slot `hasPaddedWord` already cleared, or a field stored zero
             * behind the fill that cleared it. Observed or not makes no difference: whatever read
             * the earlier one read the same zeroes. A record still standing is one no write since has
             * touched, which is what `drop` below keeps true.
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

                    if(plainWrite && !record.observed && covers(place, record.place)) {
                        detach(base, record.write);
                        kept.removeFirst(record.write - base);
                        rewrote = true;
                        drop(i - 1);
                        continue;
                    }

                    // A record this write may have changed a byte of is no longer the answer to a
                    // load, and is not one a later write can be shown to replace either.
                    if(mayOverlap(place, record.place)) drop(i - 1);
                }

                if(plainWrite && place.bytes != kUnbounded && place.bytes != 0) {
                    if(recordCount == kRecords) drop(0);
                    records[recordCount].write = inst;
                    records[recordCount].place = place;
                    records[recordCount].observed = false;
                    recordCount++;
                }
            }

            kept.push(instPtr);
        }

        if(!rewrote) continue;

        block->instructions.clear();
        for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
    }
}
