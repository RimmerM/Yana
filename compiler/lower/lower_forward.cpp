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
bool walkAddress(LowerBase base, LowerValue* address, U64 size, U64 offset, bool allowCalls, Visit&& visit) {
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

            if(!walkAddress(base, add->created().ptr, size, offset + step.unwrap(), allowCalls, visit)) {
                return false;
            }

            continue;
        }

        /*
         * §7.4 A call the storage is *handed to*, which is the one use that is not an access and is
         * still one this pass can redirect.
         *
         * A record filled by a call rather than by writes is the shape §16.2 of
         * `test/bench/findings.md` declined - `newStringOfCapacity` is given somewhere to build a
         * string and the result is then copied where it was wanted. The call is as redirectable as a
         * store: it writes through the pointer it was given, and giving it the destination instead
         * writes the same bytes in the same place. What it is *not* is analysable, so the caller
         * takes on an obligation the access cases do not have - see `addressHiddenBefore`.
         *
         * Only the argument positions. A pointer used as the callee is an address this pass has no
         * business rewriting, and `used()[0]` is where a call names it.
         */
        if(allowCalls && user->kind == LowerInst::Call) {
            auto used = user->used();
            if(used.length > 0 && used.ptr[0] == self) return false;
            if(!visit(user, offset)) return false;
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
 * §7.4.1 Every block an execution can be in and still have `target` ahead of it.
 *
 * One backward walk over the incoming lists, which answers "can this block reach `target`" for every
 * block at once - and the question is asked of many uses against one call, so that is the direction
 * to walk it in. Reachability rather than dominance, and by *at least one edge*: a block inside a
 * loop reaches itself, which is exactly the case a position comparison inside one block gets wrong.
 *
 * Over-approximate on purpose. A path this finds may be infeasible, and the answer is then a refusal
 * rather than a miscompile.
 */
void blocksReaching(LowerBase base, LowerBlock& target, HashMap<U32, U32>& into) {
    into.clear();

    SmallArray<LowerPtr<LowerBlock>, 32> pending;

    auto walk = [&](LowerBlock& from) {
        for(auto predecessor: from.incoming.contents(base)) {
            if(into.getValue(U32(predecessor))) continue;

            into.add(U32(predecessor), 1);
            pending.push(predecessor);
        }
    };

    walk(target);
    while(pending.size()) walk(*base[pending.pop().unwrap()]);
}

/*
 * §7.4 Whether a callee could hold the destination's address by any route but the one being
 * redirected into it.
 *
 * This is what replaces the aliasing argument where the writes being moved are a call's rather than
 * this function's. The destination stops being written at the copy and starts being written during
 * the call, so what has to hold is that the call cannot see it: an allocation of this frame whose
 * address has reached nothing but plain accesses by the time the call runs is one the callee has no
 * name for, whatever it does with the memory it can reach.
 *
 * "By the time" is the whole of it, and the question each use is asked is therefore **can this run
 * before the call**, not where it is written. Two things answer it, and they are the same question
 * over the two orderings a function has:
 *
 *  - a use in the call's own block runs before it exactly when it is written above it, *and* the
 *    block is not one that reaches itself. A block inside a loop reaches itself, so a use written
 *    below the call there is a use above it on the next time round.
 *  - a use anywhere else runs before the call exactly when its block can reach the call's - which is
 *    `reaching`, and is what lets `Text.showSigned` forward at all: its destination is read in five
 *    later blocks, none of which leads back to the one that builds it.
 *
 * A use is only asked at all when it could *pass the address on*; a plain access reads or writes the
 * storage and tells nobody where it is. Nor is anything downstream of a refused-or-allowed use asked
 * separately: a use of a value is reachable from that value's definition, so a block that cannot
 * reach the call has no successor that can.
 */
bool addressHiddenBefore(LowerBase base, LowerBlock& block, HashMap<U32, U32>& position,
                         HashMap<U32, U32>& reaching, LowerValue* address, Size limit) {
    auto self = address - base;
    auto here = &block - base;

    for(auto userPtr: address->uses.contents(base)) {
        auto user = base[userPtr];

        if(user->kind == LowerInst::Add) {
            // The arithmetic itself passes nothing on; what it produces is followed. An offset that
            // is not a constant is followed all the same - it still cannot leave the allocation.
            if(!addressHiddenBefore(base, block, position, reaching, user->created().ptr, limit)) {
                return false;
            }

            continue;
        }

        auto used = user->used();
        auto named = Size(0);

        for(Size which = 0;; which++) {
            auto access = accessAt(base, user, which);
            if(!access.address) break;
            if(access.address == self) named++;
        }

        auto positions = Size(0);
        for(Size i = 0; i < used.length; i++) {
            if(used.ptr[i] == self) positions++;
        }

        // A plain access reads or writes the storage and tells nobody where it is, so it is allowed
        // wherever it stands - including in another block, where it reads either the bytes the call
        // was going to be given or the ones it wrote, and the copy made those the same bytes.
        if(named > 0 && named == positions) continue;

        if(user->block != here) {
            if(reaching.getValue(U32(user->block))) return false;
            continue;
        }

        auto at = position.getValue(U32(user - base));
        if(!at || Size(at.unwrap()) <= limit) return false;
        if(reaching.getValue(U32(here))) return false;
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

    return !walkAddress(base, allocation, size.unwrap(), 0, false, [](LowerInst*, U64) { return true; });
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
                HashMap<U32, U32>& reaching, LowerInstCopy& copy) {
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

    // The calls the storage is handed to, in no order. Empty where it is written directly, which is
    // §16.2's original shape; where it is not, this is what §7.4's obligation is measured against
    // and what tells the stretch scan below which calls in it are the writes being moved.
    SmallArray<LowerInst*, 8> handedTo;
    auto lastCall = maxLimit<Size>;

    auto contained = walkAddress(base, source, size.unwrap(), 0, true, [&](LowerInst* user, U64) {
        auto at = position.getValue(U32(user - base));

        // A use in another block, or in this one's terminator or phis, is one the straight-line
        // reasoning below does not cover.
        if(!at || user->block != &block - base) {
            local = false;
            return false;
        }

        first = min(first, Size(at.unwrap()));
        last = max(last, Size(at.unwrap()));

        if(user->kind == LowerInst::Call) {
            if(!handedTo.containsValue(user)) handedTo.push(user);
            lastCall = lastCall == maxLimit<Size> ? Size(at.unwrap()) : max(lastCall, Size(at.unwrap()));
        }

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
    auto hoistCount = maxLimit<Size>;

    if(destination->inst()->kind != LowerInst::Arg) {
        auto definition = destination->inst();
        auto defined = position.getValue(U32(definition - base));
        if(!defined || definition->block != &block - base) return false;

        if(Size(defined.unwrap()) >= first) {
            if(definition->kind != LowerInst::Alloca) return false;

            auto bytes = ((LowerInstAlloca*)definition)->byteCount;
            if(constantOf(base, bytes).isNothing()) return false;

            // The size it reads has to be available where it is going, and where it is not it travels
            // with the allocation. A constant reads nothing, so moving it up cannot cross a
            // definition it depends on, and every use of it is below where it already was. One in
            // another block already dominates this one and needs nothing.
            auto counted = position.getValue(U32(base[bytes]->inst() - base));
            if(counted && Size(counted.unwrap()) >= first) hoistCount = Size(counted.unwrap());

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

    /*
     * §7.4 And where the writes being moved are a call's, the same question asked of the callee.
     *
     * It cannot be answered by looking at what the call does, so it is answered by what the call can
     * name: the destination has to be storage of this frame whose address has reached nothing but
     * plain accesses by the time the call runs. A parameter or a global is refused outright here -
     * they are exactly the destinations the escape argument above needs nothing for, and exactly the
     * ones this needs something it cannot have.
     */
    if(lastCall != maxLimit<Size>) {
        if(!destinationBase) return false;

        // Built here rather than once per function: it is one walk of the CFG, and it is wanted only
        // by the shape that gets this far - a copy whose temporary a call filled, which is a handful
        // of sites in a program and none at all in most functions.
        blocksReaching(base, block, reaching);
        if(!addressHiddenBefore(base, block, position, reaching, destinationBase, lastCall)) return false;
    }

    for(auto i = first; i < Size(here.unwrap()); i++) {
        auto inst = base[block.instructions.get(base, i)];
        if(!touchesMemory(inst)) continue;

        // A call the temporary is handed to is one of the writes being moved rather than something
        // that could reach the destination - which is what the check above has just established for
        // every one of them.
        if(inst->kind == LowerInst::Call && handedTo.containsValue(inst)) continue;

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
    // Every index moved is below the copy's - the allocation defines what the copy writes into, its
    // byte count is above the allocation, and the first write is at or above both - so the rotations
    // leave the copy where it was. The count goes first and the allocation lands behind it: moving
    // the count only shifts what lies between it and `first`, which the allocation is not among.
    if(hoistCount != maxLimit<Size>) {
        moveInstUp(base, block, hoistCount, first);
        if(hoist != maxLimit<Size>) moveInstUp(base, block, hoist, first + 1);
    } else if(hoist != maxLimit<Size>) {
        moveInstUp(base, block, hoist, first);
    }

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
    HashMap<U32, U32> reaching;

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

                if(tryForward(base, fun, *block, position, reaching, *(LowerInstCopy*)inst)) {
                    changed = true;
                    break;
                }
            }
        }
    }
}
