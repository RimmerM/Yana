#include "lower_forward.h"
#include "lower_inst.h"
#include "lower_builder.h"

namespace {

// The constant an operand carries, where it is one.
Maybe<U64> constantOf(LowerBase base, LowerPtr<LowerValue> value) {
    auto inst = base[value]->inst();
    if(inst->kind != LowerInst::Imm) return Nothing();

    return Just(((LowerImm*)inst)->i);
}

/*
 * Whether running this instruction can read or write memory.
 *
 * A whitelist of the kinds that provably cannot, rather than a list of the ones that can, so that an
 * instruction kind added later is answered "yes" and costs a rewrite rather than correctness. The
 * whole of what this pass does rests on knowing everything that could observe a destination it has
 * begun writing into, which is the one question a wrong default would answer silently.
 */
bool touchesMemory(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Arg:
        case LowerInst::Global:
        case LowerInst::Fun:
        case LowerInst::Imm:
        case LowerInst::Nop:
        case LowerInst::Select:
        case LowerInst::Alloca:
        case LowerInst::Phi:
            return false;
        default:
            return !isUnary(inst) && !isBinary(inst) && !isCast(inst);
    }
}

/*
 * One memory access, taken apart: where it goes, and how much of it.
 *
 * The four kinds that name an address name one each, except a copy, which names two - so an access
 * is a (kind, end) pair rather than an instruction, and everything below asks about accesses rather
 * than about instructions. `bytes` is zero where the extent is not a constant, which is a copy or a
 * fill of a length computed at run time and is refused wherever it appears.
 */
struct Access {
    LowerPtr<LowerValue> address = nullptr;
    U64 bytes = 0;
};

Access accessAt(LowerBase base, LowerInst* inst, Size which) {
    auto extent = [&](LowerPtr<LowerValue> count) {
        auto value = constantOf(base, count);
        return value ? value.unwrap() : U64(0);
    };

    switch(inst->kind) {
        case LowerInst::Load:
            if(which > 0) return {};
            return { ((LowerInstLoad*)inst)->from, ((LowerInstLoad*)inst)->getWidth() };
        case LowerInst::Store:
            if(which > 0) return {};
            return { ((LowerInstStore*)inst)->to, ((LowerInstStore*)inst)->getWidth() };
        case LowerInst::SetPattern:
            if(which > 0) return {};
            return { ((LowerInstSetPattern*)inst)->to, extent(((LowerInstSetPattern*)inst)->count) };
        case LowerInst::Copy:
            if(which > 1) return {};
            return {
                which == 0 ? ((LowerInstCopy*)inst)->to : ((LowerInstCopy*)inst)->from,
                extent(((LowerInstCopy*)inst)->count),
            };
        default:
            return {};
    }
}

/*
 * Every use of an address, checked for being one this pass can account for and account for
 * completely.
 *
 * This is the single statement of what "the address does not escape" means here, and both questions
 * below are it asked with a different visitor: the temporary's own closure, which the rewrite has to
 * be able to redirect in full, and the allocation an unrelated access resolves to, which only has to
 * be shown to be nobody else's.
 *
 * Three uses are accounted for. An access *through* it - a load, a store, a fill, a copy at either
 * end - is what the storage is for; a constant offset from it is a further address, and is followed.
 * Everything else is the address leaving, and is the answer "no": as a call argument, as a value
 * stored into memory, as a copy's byte count, as an operand of arithmetic that is not a constant
 * offset, as a phi alternative, as the operand of a terminator.
 *
 * The offset is carried so that each access is known to land inside the allocation. An access that
 * runs past the end of it, or an offset that is not a constant, is refused rather than trusted: it
 * is the one thing that would put a redirected write outside the bytes the copy was going to move.
 */
template<class Visit>
bool walkAddress(LowerBase base, LowerValue* address, U64 size, U64 offset, Visit&& visit) {
    auto self = address - base;

    for(auto userPtr: address->uses.contents(base)) {
        auto user = base[userPtr];

        if(user->kind == LowerInst::Add) {
            auto add = (LowerInstBinary*)user;

            // Either operand may be the address: nothing has canonicalized the order yet, and a
            // pointer add is written both ways round by different parts of lowering.
            auto step = add->lhs == self ? constantOf(base, add->rhs)
                      : add->rhs == self ? constantOf(base, add->lhs)
                      : Nothing();

            if(step.isNothing() || step.unwrap() >= size - offset) return false;
            if(!visit(user, offset)) return false;

            if(!walkAddress(base, add->created().ptr, size, offset + step.unwrap(), visit)) {
                return false;
            }

            continue;
        }

        // An address operand of an access, and only that. The same instruction naming it in any
        // other position - a pointer stored as a value, a pointer handed over as a byte count - is
        // the address reaching somewhere this pass cannot follow it.
        auto used = user->used();
        auto named = Size(0);

        for(Size which = 0;; which++) {
            auto access = accessAt(base, user, which);
            if(!access.address) break;
            if(access.address != self) continue;

            if(access.bytes == 0 || access.bytes > size - offset) return false;
            named++;
        }

        auto positions = Size(0);
        for(Size i = 0; i < used.length; i++) {
            if(used.ptr[i] == self) positions++;
        }

        if(named == 0 || named != positions) return false;
        if(!visit(user, offset)) return false;
    }

    return true;
}

/*
 * The allocation a pointer is part of, where it is one - and null for every pointer that came from
 * somewhere this pass cannot name.
 *
 * This is one half of the aliasing argument, and the half that needs no analysis at all. Two
 * distinct allocations are disjoint by construction, so an access this resolves to one allocation
 * cannot reach a destination that resolves to another.
 */
LowerValue* allocationBase(LowerBase base, LowerPtr<LowerValue> pointer) {
    auto value = base[pointer];

    // Down to the allocation through the constant offsets, which is the chain walkAddress climbs the
    // other way. Bounded rather than followed to the end: a pointer this far from its allocation is
    // not a shape lowering emits, and a budget is what keeps one question from walking a dataflow
    // graph.
    for(auto steps = 0; steps < 8; steps++) {
        auto inst = value->inst();

        if(inst->kind == LowerInst::Alloca) {
            return constantOf(base, ((LowerInstAlloca*)inst)->byteCount) ? value : nullptr;
        }

        if(inst->kind != LowerInst::Add) return nullptr;

        auto add = (LowerInstBinary*)inst;
        if(constantOf(base, add->rhs).isJust()) value = base[add->lhs];
        else if(constantOf(base, add->lhs).isJust()) value = base[add->rhs];
        else return nullptr;
    }

    return nullptr;
}

/*
 * Whether anything outside this function could hold a pointer into this allocation.
 *
 * The other half of the aliasing argument, and the half only one destination needs. An allocation
 * that is only ever loaded through, stored through, copied at either end, filled, or offset by a
 * constant to do one of those, has had its address given to nothing - so no pointer that arrived
 * from elsewhere can be it.
 */
bool allocationEscapes(LowerBase base, LowerValue* allocation) {
    auto size = constantOf(base, ((LowerInstAlloca*)allocation->inst())->byteCount);
    if(size.isNothing()) return true;

    return !walkAddress(base, allocation, size.unwrap(), 0, [](LowerInst*, U64) { return true; });
}

// Where each instruction of a block sits in it, so that "before the copy" is a comparison rather than
// a search. Rebuilt whenever a rewrite has taken instructions out of the list.
void indexBlock(LowerBase base, LowerBlock& block, HashMap<U32, U32>& into) {
    into.clear();

    Size at = 0;
    for(auto instPtr: block.instructions.contents(base)) into.add(U32(instPtr), U32(at++));
}

// Moving one instruction of a block up to an earlier position in it, by rotating the entries between
// the two. The list has no insert, and this needs none: nothing is added or removed.
void moveInstUp(LowerBase base, LowerBlock& block, Size from, Size to) {
    auto moved = block.instructions.get(base, from);

    for(auto i = from; i > to; i--) {
        block.instructions.set(base, i, block.instructions.get(base, i - 1));
    }

    block.instructions.set(base, to, moved);
}

// Taking one instruction out of the block it is in, wherever that is. An allocation this pass
// removes is usually in the block whose copies it was serving and is not required to be.
void eraseInst(LowerBase base, LowerInst* inst) {
    detach(base, inst);

    auto block = base[inst->block];
    for(Size i = 0; i < block->instructions.size(); i++) {
        if(base[block->instructions.get(base, i)] == inst) {
            block->instructions.remove(base, i);
            return;
        }
    }
}

/*
 * One copy, and whether the value it moves could have been built where it was going.
 *
 * Everything the header lists is checked here, in the order that makes the cheap refusals cheap: the
 * shape of the copy, then the temporary's own uses, then the stretch between the first of them and
 * the copy. The last is the only part that walks instructions the copy does not name.
 */
bool tryForward(LowerBase base, LowerFunction& fun, LowerBlock& block, HashMap<U32, U32>& position,
                LowerInstCopy& copy) {
    auto source = base[copy.from];
    auto destination = base[copy.to];
    if(source == destination) return false;

    // The whole of an allocation, and nothing more or less than it. A count that is not the
    // allocation's own leaves bytes behind on one side or the other.
    auto allocation = source->inst();
    if(allocation->kind != LowerInst::Alloca) return false;

    auto size = constantOf(base, ((LowerInstAlloca*)allocation)->byteCount);
    auto count = constantOf(base, copy.count);
    if(size.isNothing() || count.isNothing()) return false;
    if(size.unwrap() == 0 || size.unwrap() != count.unwrap()) return false;

    auto here = position.getValue(U32((LowerInst*)&copy - base));
    if(!here) return false;

    /*
     * Every use of the temporary, which the rewrite has to be able to redirect in full. `first` and
     * `last` come out of the same walk: the copy has to be the last of them, since the writes are
     * what move forward to its position, and `first` is where the stretch below begins. The offsets
     * are visited too, so that an address computed in another block - where the destination might
     * not yet be available - is one of the things this refuses.
     */
    auto first = maxLimit<Size>;
    auto last = Size(0);
    auto local = true;

    auto contained = walkAddress(base, source, size.unwrap(), 0, [&](LowerInst* user, U64) {
        auto at = position.getValue(U32(user - base));

        // A use in another block, or in this one's terminator or phis, is one the straight-line
        // reasoning below does not cover.
        if(!at || user->block != &block - base) {
            local = false;
            return false;
        }

        first = min(first, Size(at.unwrap()));
        last = max(last, Size(at.unwrap()));
        return true;
    });

    if(!contained || !local) return false;
    if(last != Size(here.unwrap())) return false;

    /*
     * The destination has to exist where the first of those writes is, since that is where it starts
     * being written. A parameter is available throughout; anything else has to be defined above.
     *
     * An allocation that is not is *moved* above, which is the one thing here that rewrites
     * something other than the copy - and it is free rather than a trade. A fixed allocation is a
     * frame object that exists for the whole of the function wherever it was written; the
     * instruction is only what names its address, and it reads a constant. So the position is
     * arbitrary, and this is the shape it matters for: a record built in one temporary and copied
     * into a second because the second is what a call is about to be handed.
     *
     * A dynamic allocation is excluded by needing a constant size, and has to be: that one moves the
     * stack pointer, and where it does so is the whole of what it says.
     */
    auto hoist = maxLimit<Size>;

    if(destination->inst()->kind != LowerInst::Arg) {
        auto definition = destination->inst();
        auto defined = position.getValue(U32(definition - base));
        if(!defined || definition->block != &block - base) return false;

        if(Size(defined.unwrap()) >= first) {
            if(definition->kind != LowerInst::Alloca) return false;

            auto bytes = ((LowerInstAlloca*)definition)->byteCount;
            if(constantOf(base, bytes).isNothing()) return false;

            // The size it reads has to be available where it is going. A constant defined in another
            // block already dominates this one, so only one in this block can be in the way.
            auto counted = position.getValue(U32(base[bytes]->inst() - base));
            if(counted && Size(counted.unwrap()) >= first) return false;

            hoist = Size(defined.unwrap());
        }
    }

    /*
     * The stretch the writes move over, and the one thing that can go wrong in it: something that
     * can see the destination. Every access in it has to resolve to an allocation - which is what a
     * call cannot do, and what an access through a pointer of unknown origin cannot do either - and
     * to one that is not the allocation the destination is part of.
     *
     * Where the destination is *not* an allocation, one more thing decides it, and it is the one
     * place an escape analysis is needed. A parameter and a global cannot be storage this frame
     * created, so an access to any allocation of this frame is disjoint from either and nothing has
     * to be proved. Anything else - a pointer a call returned, one read out of memory - could be an
     * allocation whose address this function handed over, so each of them has to be shown not to
     * have been.
     *
     * The temporary's own accesses pass this without being named as a case: they resolve to the
     * temporary, which is an allocation and is not the destination, and they are the writes being
     * moved.
     */
    auto destinationBase = allocationBase(base, copy.to);
    auto destinationKind = destination->inst()->kind;
    auto destinationOutside = destinationKind == LowerInst::Arg || destinationKind == LowerInst::Global;

    for(auto i = first; i < Size(here.unwrap()); i++) {
        auto inst = base[block.instructions.get(base, i)];
        if(!touchesMemory(inst)) continue;

        auto known = false;
        for(Size which = 0;; which++) {
            auto access = accessAt(base, inst, which);
            if(!access.address) break;

            auto owner = allocationBase(base, access.address);
            if(!owner) return false;

            if(destinationBase) {
                // Two allocations, and two of those are disjoint unless they are the same one.
                if(owner == destinationBase) return false;
            } else if(!destinationOutside) {
                // A pointer of this function's own making, which could be an allocation it gave the
                // address of away.
                if(allocationEscapes(base, owner)) return false;
            }

            known = true;
        }

        // Something that touches memory and names no address this pass understands - a call, an
        // intrinsic, an instruction kind added later. It may reach the destination, so this stops.
        if(!known) return false;
    }

    /*
     * The rewrite. Every use of the temporary becomes a use of the destination, which turns each
     * write into the write the copy was going to perform and each offset into the same offset from
     * the other address; the copy then has nothing left to move, and the allocation nothing in it.
     */
    // Both indices are below the copy's - the allocation defines what the copy writes into, and the
    // first write is at or above it - so the rotation leaves the copy where it was.
    if(hoist != maxLimit<Size>) moveInstUp(base, block, hoist, first);

    detach(base, (LowerInst*)&copy);
    block.instructions.remove(base, Size(here.unwrap()));

    replaceUses(base, fun.module->arena, source - base, copy.to);
    eraseInst(base, allocation);

    // The destination now holds what the temporary held, so it has to be aligned as the temporary
    // was. Only an allocation can be told so; a pointer from outside already satisfies whatever its
    // own storage promised, which is what let the copy write there at all.
    if(destination->inst()->kind == LowerInst::Alloca) {
        auto target = (LowerInstAlloca*)destination->inst();
        target->alignment = max(target->alignment, ((LowerInstAlloca*)allocation)->alignment);
    }

    return true;
}

}

void forwardCopyDestinations(LowerBase base, LowerFunction& fun) {
    HashMap<U32, U32> position;

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        /*
         * From the bottom up, so that a chain of temporaries collapses without needing a walk per
         * link: forwarding the copy nearest the end gives the copy above it the destination the
         * whole chain was heading for, and that copy is the next one this reaches.
         *
         * The scan restarts after a rewrite rather than resuming, because a rewrite removes two
         * instructions from the list and the positions every later question is asked in terms of
         * are the ones it invalidates. Restarting is what makes that a reindex rather than an
         * argument about which indices moved.
         */
        auto changed = true;
        while(changed) {
            changed = false;
            indexBlock(base, *block, position);

            for(auto i = block->instructions.size(); i-- > 0;) {
                auto inst = base[block->instructions.get(base, i)];
                if(inst->kind != LowerInst::Copy) continue;

                if(tryForward(base, fun, *block, position, *(LowerInstCopy*)inst)) {
                    changed = true;
                    break;
                }
            }
        }
    }
}
