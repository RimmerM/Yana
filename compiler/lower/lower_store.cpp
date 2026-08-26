#include "lower_store.h"
#include "lower_fold.h"
#include "lower_builder.h"

namespace {

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

} // namespace

void forwardStoredValues(LowerBase base, LowerModule& module, LowerFunction& fun) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        // Inline: one of these per block, holding the instructions of that block while the list it
        // came from is rewritten - the same shape as foldFunctionConstants, and for the same reason.
        SmallArray<LowerPtr<LowerInst>, 32> kept;
        auto rewrote = false;

        /*
         * The one store the walk is still inside the answer to, and whether anything since it could
         * have read what it wrote. The second is what decides whether a store to the same pointer
         * overwrites a word nothing observed, which is the other half of the pair - `%c` becoming
         * `%b` is what makes the store that produced it dead.
         */
        LowerInstStore* pending = nullptr;
        auto observed = false;

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            if(inst->kind == LowerInst::Load && pending) {
                if(auto value = forwardedValue(base, pending, (LowerInstLoad*)inst)) {
                    detach(base, inst);
                    replaceUses(base, module.arena, ((LowerInstSingle*)inst)->created().ptr - base,
                                value - base);
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
                if(pending && !observed && pending->to == store->to &&
                   pending->getWidth() == store->getWidth())
                {
                    detach(base, (LowerInst*)pending);
                    kept.removeFirst((LowerInst*)pending - base);
                    rewrote = true;
                }

                pending = store;
                observed = false;
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
            if(hasLowerTrait(inst, kLowerWrites)) pending = nullptr;
            else if(hasLowerTrait(inst, kLowerReads)) observed = true;

            kept.push(instPtr);
        }

        if(!rewrote) continue;

        block->instructions.clear();
        for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
    }
}
