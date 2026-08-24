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
        case LowerInst::VecSplat:
        case LowerInst::VecLane:
        case LowerInst::VecWithLane:
        case LowerInst::VecShuffle:
        case LowerInst::VecReduce:

        // `Fma` is neither unary nor binary, so the fallback below answers *yes* for it - which is
        // this function's stated design (a new kind touches memory until it says otherwise) and is
        // wrong here. `Sqrt` reaches the fallback correctly, being a Unary.
        case LowerInst::Fma:

        // The SHA rounds, here for `Fma`'s reason: `Sha256Rounds` has three operands and is neither
        // unary nor binary, so the fallback would answer *yes* for it. `ShaBinary` is a binary and
        // would reach the fallback correctly; it is named beside its sibling so that the two are not
        // separated by a rule neither of them states.
        case LowerInst::ShaBinary:
        case LowerInst::Sha256Rounds:
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
         * `test/bench/findings.md` declined - `String.ofCapacity` is given somewhere to build a
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
 * §27 Whether this instruction is a call to the program's own heap allocator.
 *
 * The one callee a pass at this tier is entitled to know anything about, and it knows it because the
 * compiler wrote the call: an `InstAlloc` the escape analysis placed on the heap is lowered to a
 * call to `Native.allocateHeap` and to nothing else, so the callee is `LowerModule::allocator`
 * exactly when the storage it hands back is storage this compilation chose to put there.
 *
 * The callee is `used()[0]`, and only a direct one counts - a pointer through a variable names a
 * function this cannot see, and the same name reached by an indirect call is not the same fact.
 */
bool isAllocatorCall(LowerBase base, LowerFunction& fun, LowerInst* inst) {
    auto allocator = fun.module->allocator;
    if(!allocator || inst->kind != LowerInst::Call) return false;

    auto used = inst->used();
    if(used.length == 0) return false;

    auto callee = base[used.ptr[0]]->inst();
    if(callee->kind != LowerInst::Fun) return false;

    return ((LowerInstFun*)callee)->target == allocator;
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
                HashMap<U32, U32>& reaching, LowerPtr<LowerValue> returnPlace, LowerInstCopy& copy) {
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
     *
     * §27 The second thing that may move is a call to the heap allocator, and it is not free: it
     * exchanges the order of two calls. `isAllocatorCall` is what recognizes one and lower_forward.h
     * is the whole of what makes the exchange admissible; note that the two cases meet here rather
     * than anywhere lower down, because what has to hold about the *position* is one statement.
     */
    // Ascending, and every entry at or below `first`, so that the rewrite can lift them in order.
    SmallArray<Size, 4> lift;
    auto definition = destination->inst();
    auto movesAllocator = false;

    if(definition->kind != LowerInst::Arg) {
        auto defined = position.getValue(U32(definition - base));
        if(!defined || definition->block != &block - base) return false;

        if(Size(defined.unwrap()) >= first) {
            movesAllocator = isAllocatorCall(base, fun, definition);

            if(definition->kind == LowerInst::Alloca) {
                if(constantOf(base, ((LowerInstAlloca*)definition)->byteCount).isNothing()) return false;
            } else if(!movesAllocator) {
                return false;
            }

            /*
             * What it reads has to be available where it is going, and where it is not it travels
             * with it. Only an operand that performs nothing and reads nothing may travel - a
             * constant, a function's address, a global's - so moving it up cannot cross a definition
             * it depends on, and every use of it is below where it already was. One in another block
             * already dominates this one and needs nothing.
             *
             * The allocator call is why this is a list rather than the single byte count it was:
             * lowering writes the `Fun` naming the callee directly in front of the call, so a call
             * being lifted always has at least one operand standing between it and where it is
             * going.
             */
            for(auto use: definition->used()) {
                auto operand = base[use]->inst();
                auto at = position.getValue(U32(operand - base));
                if(!at || Size(at.unwrap()) < first) continue;

                auto kind = operand->kind;
                if(kind != LowerInst::Imm && kind != LowerInst::Fun && kind != LowerInst::Global) {
                    return false;
                }

                // Kept ascending as it is built, which for a handful of operands is one comparison
                // each and saves the rewrite below having to sort anything.
                auto entry = Size(at.unwrap());
                if(lift.containsValue(entry)) continue;

                lift.push(entry);
                for(auto i = lift.size() - 1; i > 0 && lift[i - 1] > lift[i]; i--) {
                    ::swap(lift[i - 1], lift[i]);
                }
            }

            // Last, and correctly so: an operand is always defined above the instruction reading it.
            lift.push(Size(defined.unwrap()));
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
    auto destinationKind = definition->kind;
    auto destinationOutside = destinationKind == LowerInst::Arg || destinationKind == LowerInst::Global;

    /*
     * §27 A heap block this function has just been handed is an allocation in exactly the sense the
     * paragraph above uses the word, and stating that is what lets the destination be one.
     *
     * It is fresh: the allocator either takes it off a free list or cuts it out of the bump area, and
     * either way nothing that is live holds a pointer into it. So no access in the stretch can reach
     * it - which is the same conclusion "two distinct allocations are disjoint" reaches for two
     * `alloca`s, arrived at from the allocator's contract rather than from the frame's layout.
     *
     * Only where it is the destination. The reverse - admitting one as the `owner` of some unrelated
     * access below - is *not* sound, and the difference is worth stating: two `alloca`s are distinct
     * storage for the whole of a frame, while two heap blocks are only distinct while both are live,
     * and nothing here establishes that the first was not freed before the second was taken.
     */
    if(!destinationBase && isAllocatorCall(base, fun, definition)) destinationBase = destination;

    /*
     * §7.4 And where the writes being moved are a call's, the same question asked of the callee.
     *
     * It cannot be answered by looking at what the call does, so it is answered by what the call can
     * name: the destination has to be storage of this frame whose address has reached nothing but
     * plain accesses by the time the call runs. A global and every parameter but one are refused
     * outright here - they are exactly the destinations the escape argument above needs nothing for,
     * and exactly the ones this needs something it cannot have. The one is this function's own hidden
     * result pointer, which is answered below.
     */
    if(lastCall != maxLimit<Size>) {
        /*
         * The one destination outside this frame that answers it, and the answer is the caller's
         * rather than anything provable here: a hidden result pointer names storage that call site
         * allocated for this call and nothing else - see lower_forward.h - so the callee being handed
         * it has no second route to it. Every other parameter and every global is refused, as before.
         */
        if(!destinationBase) {
            if(copy.to != returnPlace || destinationKind != LowerInst::Arg) return false;
        } else {
            // Built here rather than once per function: it is one walk of the CFG, and it is wanted
            // only by the shape that gets this far - a copy whose temporary a call filled, which is
            // a handful of sites in a program and none at all in most functions.
            blocksReaching(base, block, reaching);
            if(!addressHiddenBefore(base, block, position, reaching, destinationBase, lastCall)) {
                return false;
            }
        }
    }

    for(auto i = first; i < Size(here.unwrap()); i++) {
        auto inst = base[block.instructions.get(base, i)];
        if(!touchesMemory(inst)) continue;

        // A call the temporary is handed to is one of the writes being moved rather than something
        // that could reach the destination - which is what the check above has just established for
        // every one of them.
        if(inst->kind == LowerInst::Call && handedTo.containsValue(inst)) continue;

        // §27 And the destination's own definition is not in this stretch once the rewrite has run,
        // since lifting it above `first` is the rewrite. It is the only call that leaves this way.
        if(movesAllocator && inst == definition) continue;

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
    // Every index moved is below the copy's - the definition produces what the copy writes into, what
    // that definition reads is above it, and the first write is at or above all of them - so the
    // rotations leave the copy where it was. Taken in ascending order and landing in that same order
    // just above `first`, each move shifts only what lies between its own source and destination, so
    // the entries still to come keep the positions they were collected at.
    for(Size i = 0; i < lift.size(); i++) moveInstUp(base, block, lift[i], first + i);

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

void forwardCopyDestinations(LowerBase base, LowerFunction& fun, LowerPtr<LowerValue> returnPlace) {
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

                if(tryForward(base, fun, *block, position, reaching, returnPlace, *(LowerInstCopy*)inst)) {
                    changed = true;
                    break;
                }
            }
        }
    }
}
