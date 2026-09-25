#include "lower_teardown.h"
#include "lower_builder.h"
#include "lower_store.h"

/*
 * See lower_teardown.h for when a teardown call is replaced by its body. This file is the two
 * questions - is the callee's body one that can be copied, and is the subject a slot the split will
 * take - and the copy.
 */

namespace {

/*
 * The block a body's walk is in: the only one, or - before the callee's own pipeline has merged its
 * blocks, which is when a caller asks, since generated functions are optimized after every declared
 * one - the block the entry jumps straight to (`LowerFunction::addBlock`).
 */
LowerBlock* walkBlock(LowerBase base, LowerFunction& callee) {
    if(callee.blocks.size() == 1) return base[callee.blocks.get(base, 0)];
    if(callee.blocks.size() != 2) return nullptr;

    auto entry = base[callee.blocks.get(base, 0)];
    if(!entry->terminator || entry->instructions.size() || entry->phis.size()) return nullptr;

    auto jump = base[entry->terminator];
    if(jump->kind != LowerInst::Jmp) return nullptr;

    auto body = callee.blocks.get(base, 1);
    return ((LowerInstJmp*)jump)->then == body ? base[body] : nullptr;
}

/*
 * Whether a teardown's body is a walk this file copies: one block, one pointer argument, and nothing
 * but literals, address arithmetic, loads and calls that answer nothing. `teardownHalves` writes
 * exactly that for a plain record; anything else - a branch, a store, a call with a result - is a
 * body this leaves behind its call.
 */
bool copyableBody(LowerBase base, LowerFunction& callee) {
    if(!callee.teardown || callee.args.size() != 1) return false;

    auto block = walkBlock(base, callee);
    if(!block || block->phis.size()) return false;

    for(auto instPtr: block->instructions.contents(base)) {
        auto inst = base[instPtr];

        switch(inst->kind) {
            case LowerInst::Imm:
            case LowerInst::Fun:
            case LowerInst::Add:
                break;
            case LowerInst::Load:
                if(((LowerInstLoad*)inst)->isVolatile()) return false;
                break;
            case LowerInst::Call:
                if(inst->createdCount != 0) return false;
                break;
            default:
                return false;
        }
    }

    // A body not written yet has no terminator: a declared teardown - an iterator object's, say -
    // is lowered in module order, which may be behind this caller.
    if(!block->terminator) return false;

    auto terminator = base[block->terminator];
    return terminator->kind == LowerInst::Ret && terminator->usedCount == 0;
}

/*
 * Whether a slot the body fills with stores and reads field by field is one the teardown's copy lets
 * go: every use of the address, and of every constant offset from it, is a store into it, a load out
 * of it - or `call` itself.
 *
 * That is a record whose fields were in registers until they were stored for the call: a call's
 * result, a `->` parameter's fields, a record assembled from values. Copying the body in leaves the
 * slot nothing but stores and loads, which the split and the promotion then turn back into the
 * registers the stores came from, and the slot is gone.
 *
 * **Not a slot a copy names.** The first form admitted every use `addressContained` does - copies
 * as well - and cost `test/base` 2.4 KB for the same instruction count: `at: Site` arrives behind a
 * pointer, is copied into a slot, and is copied again into a `Failure` on the arm that reports, so
 * the split loaded every field at the entry and kept them across that arm's calls to hand them to
 * two `String.reclaim`s on the other one. Bytes that arrive or leave by a copy are bytes in memory,
 * and one call reading them there is cheaper than the registers that would hold them to spare it.
 */
bool containedButFor(LowerBase base, LowerValue* address, LowerInst* call, U32 depth) {
    if(depth >= 8) return false;
    auto self = address - base;

    for(auto userPtr: address->uses.contents(base)) {
        auto user = base[userPtr];
        if(user == call) continue;

        switch(user->kind) {
            case LowerInst::Load:
                break;
            case LowerInst::Store:
                if(((LowerInstStore*)user)->value == self) return false;
                break;
            case LowerInst::Add: {
                auto add = (LowerInstBinary*)user;
                auto other = base[add->lhs == self ? add->rhs : add->lhs]->inst();
                if(add->lhs == add->rhs || !other || other->kind != LowerInst::Imm) return false;
                if(((LowerImm*)other)->i > (U64(1) << 32)) return false;
                if(!containedButFor(base, &add->result, call, depth + 1)) return false;
                break;
            }
            default:
                return false;
        }
    }

    return true;
}

// The slot a teardown's subject is, where it is a frame slot of this function at a constant offset.
LowerValue* subjectSlot(LowerBase base, LowerPtr<LowerValue> address) {
    auto place = placeOf(base, address, 0);
    if(!place.root || !place.allocation) return nullptr;
    return place.root;
}

struct Expander {
    LowerBase base;
    LowerModule& module;

    // The callee's values mapped to the copies, by the callee value's offset.
    HashMap<U32, LowerPtr<LowerValue>> mapped;

    LowerPtr<LowerValue> map(LowerPtr<LowerValue> value) {
        auto found = mapped.getValue(U32(value.offset));
        assertTrue(found.isJust()); // a copied instruction reads a value the copy has not made
        return found.unwrap();
    }

    // One instruction of the callee, copied to the end of `block`; its result is recorded in `mapped`.
    void copy(LowerBlock& block, LowerInst* from) {
        LowerInstSingle* made = nullptr;

        switch(from->kind) {
            case LowerInst::Imm: {
                auto imm = (LowerImm*)from;
                auto copied = new (module.arena) LowerImm(StringId(), imm->result.type, U64(0));
                copied->i = imm->i;
                made = copied;
                break;
            }
            case LowerInst::Fun:
                made = new (module.arena) LowerInstFun(StringId(), ((LowerInstFun*)from)->target);
                break;
            case LowerInst::Add: {
                auto add = (LowerInstBinary*)from;
                made = new (module.arena) LowerInstBinary(StringId(), add->result.type, map(add->lhs),
                                                          map(add->rhs), LowerInst::Add);
                break;
            }
            case LowerInst::Load: {
                auto load = (LowerInstLoad*)from;
                auto copied = new (module.arena) LowerInstLoad(map(load->from), StringId(),
                                                               load->result.type, 0, false);
                copied->flags = load->flags;
                made = copied;
                break;
            }
            case LowerInst::Call: {
                auto used = from->used();
                auto built = call(base, module, block, 0, used.length, ((LowerInstCall*)from)->getCallType(),
                                  [&](LowerInstCall* made) {
                    for(Size i = 0; i < used.length; i++) made->used()[i] = map(used.ptr[i]);
                });
                built->source = from->source;
                return;
            }
            default:
                assertTrue(false); // `copyableBody` admitted a kind this does not copy
                return;
        }

        made->source = from->source;
        block.addInst(base, made);
        mapped.add(U32((&((LowerInstSingle*)from)->result - base).offset), &made->result - base);
    }

    /*
     * The call at `index` of `block` replaced by the callee's body. The copies are appended and then
     * moved to where the call was, in one rebuild of the list; the call leaves, and its `fun` with it
     * once nothing else reads that (`removeDeadValues` behind).
     */
    void expand(LowerBlock& block, Size index, LowerInst* callInst, LowerFunction& callee) {
        mapped.clear();

        auto calleeBlock = walkBlock(base, callee);
        auto argument = &base[callee.args.get(base, 0)]->result;
        mapped.add(U32((argument - base).offset), callInst->used()[1]);

        auto before = block.instructions.size();
        for(auto instPtr: calleeBlock->instructions.contents(base)) copy(block, base[instPtr]);
        auto after = block.instructions.size();

        SmallArray<LowerPtr<LowerInst>, 64> order;
        for(Size i = 0; i < before; i++) {
            if(i == index) {
                for(Size k = before; k < after; k++) order.push(block.instructions.get(base, k));
                continue;
            }

            order.push(block.instructions.get(base, i));
        }

        detach(base, callInst);

        block.instructions.clear();
        for(auto instPtr: order) block.instructions.push(module.arena, instPtr);
    }

    bool run(LowerFunction& fun) {
        auto changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            Size i = 0;
            while(i < block->instructions.size()) {
                auto inst = base[block->instructions.get(base, i)];
                auto callee = directCallee(base, inst);
                auto copyable = callee && inst->usedCount == 2 && copyableBody(base, *callee);
                auto slot = copyable ? subjectSlot(base, inst->used()[1]) : nullptr;

                if(!slot || !containedButFor(base, slot, inst, 0)) {
                    i++;
                    continue;
                }

                // On past the copies, which are this callee's walk and hold no call of the kind
                // asked about: a teardown's body calls a field's teardown, never a record's.
                auto copies = walkBlock(base, *callee)->instructions.size();
                expand(*block, i, inst, *callee);
                i += copies;
                changed = true;
            }
        }

        return changed;
    }
};

} // namespace

void expandTeardownCalls(LowerBase base, LowerModule& module, LowerFunction& fun) {
    Expander expander {base, module};
    if(expander.run(fun)) removeDeadValues(base, module.arena, fun);
}
