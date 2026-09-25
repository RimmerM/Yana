#include "lower_forward.h"
#include "lower_inst.h"
#include "lower_builder.h"
#include "lower_store.h"

namespace {

/*
 * How far one access reaches, as a number this pass can use.
 *
 * The instruction states its own extent - see `LowerInst::accessAt` - as a width where it has one
 * and as an operand where the length is computed. Zero is the answer for a length that is not a
 * constant, and every use of it below is a refusal: an access whose extent is unknown is one that
 * cannot be shown to land inside the allocation.
 */
U64 accessBytes(LowerBase base, const LowerAccess& access) {
    if(!access.count) return access.bytes;

    auto value = constantOf(base, access.count);
    return value ? value.unwrap() : U64(0);
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
 * Three uses are accounted for. An access *through* it - a load, a store, an atomic, a fill, a copy
 * at either end, whatever else declares one - is what the storage is for; a constant offset from it
 * is a further address, and is followed.
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

        LowerAccess accesses[kMaxAccesses];
        auto count = lowerInstAccesses(user, accesses);

        for(Size which = 0; which < count; which++) {
            if(*accesses[which].address != self) continue;

            auto bytes = accessBytes(base, accesses[which]);
            if(bytes == 0 || bytes > size - offset) return false;
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

        LowerAccess accesses[kMaxAccesses];
        auto count = lowerInstAccesses(user, accesses);

        for(Size which = 0; which < count; which++) {
            if(*accesses[which].address == self) named++;
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
    // `placeOf` (lower_store.h) is the walk down to the root - J163; this used to be a copy of it that
    // took any immediate as an offset. What is asked here is only whether the root is an allocation
    // of a known size, so the walk needs no `contained` table.
    auto place = placeOf(base, pointer, 0);
    if(!place.allocation) return nullptr;

    return constantOf(base, ((LowerInstAlloca*)place.root->inst())->byteCount) ? place.root : nullptr;
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
    if(constantOf(base, ((LowerInstAlloca*)allocation->inst())->byteCount).isNothing()) return true;

    // The question `addressContained` (lower_store.h) answers for `placeOf`'s `contained`, so it is
    // asked there - J163. This used to be `walkAddress` with a visitor accepting everything, which
    // differs in two places and neither is about escaping: it refused a constant offset past the
    // end of the allocation, which is not the address going anywhere, and it accepted every kind
    // declaring the address as an access - an atomic among them - which `addressContained` does not.
    return !addressContained(base, allocation, 0);
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
 * What one rewrite did to the block `forwardCopies` is scanning, so that the scan can carry on from the
 * copy it rewrote rather than starting the block again - J166.
 *
 * `touched` is the lowest index the rewrite changed: every instruction it removed or rotated is at or
 * above it and at or below the copy, so the positions from there to the copy are the only ones that
 * have to be taken again. The ones below the copy keep the numbers they had, which are too large by
 * what was removed and therefore still in order with everything above - and order is all that is
 * asked of them. `kept` says whether the copy is still in the list (a relay leaves it, reading from
 * the relay's source).
 *
 * An allocation the rewrite empties is detached at once and taken out of this block's list only once
 * the block is done, since its index is usually near the top and removing it would renumber the whole
 * block; one in another block is erased as before. An `alloca` has no storage traits, so the scans
 * over a stretch step over one that is waiting to go.
 */
struct BlockEdits {
    LowerPtr<LowerBlock> block;
    SmallArray<LowerInst*, 16> retired;
    Size touched = 0;
    bool kept = false;

    void retire(LowerBase base, LowerInst* allocation) {
        if(allocation->block != block) {
            eraseInst(base, allocation);
            return;
        }

        detach(base, allocation);
        retired.push(allocation);
    }

    // The retired allocations leave the list in one pass - `eraseInst` is a search of the block each.
    void flush(LowerBase base) {
        if(retired.size() == 0) return;

        HashMap<U32, U8> gone;
        for(auto inst: retired) gone.add(U32(inst - base), 1);

        auto list = &base[block]->instructions;
        auto count = list->size();
        Size kept = 0;

        for(Size i = 0; i < count; i++) {
            auto instPtr = list->get(base, i);
            if(gone.getValue(U32(instPtr))) continue;
            if(kept != i) list->set(base, kept, instPtr);
            kept++;
        }

        for(auto i = count; i-- > kept;) list->remove(base, i);
        retired.clear();
    }
};

/*
 * Every use of a slot inside the block that hands it over, so that the edge is the last thing that
 * reads it.
 *
 * A walk over the names an address has, on `walkAddress`'s recursion and with its refusal: an
 * address this cannot follow is one whose readers it cannot enumerate. Two differences, and both are
 * about what is being proved. The phi is allowed, because handing the slot over is the whole point
 * of the shape; and the *position* of a use is not looked at, only its block, because what has to
 * hold is that no execution reads the slot after the edge and every use inside the predecessor is
 * above the terminator by construction.
 */
bool tiedToBlock(LowerBase base, LowerValue* address, LowerInst* phi, LowerPtr<LowerBlock> block,
                 Size depth) {
    // The chains this asks about are a constant offset or two off a record header; a bound rather
    // than a limit, and past it the rewrite is declined.
    if(depth > 8) return false;

    for(auto userPtr: address->uses.contents(base)) {
        auto user = base[userPtr];
        if(user == phi) continue;
        if(user->block != block) return false;

        if(user->kind == LowerInst::Add) {
            if(!tiedToBlock(base, ((LowerInstBinary*)user)->created().ptr, phi, block, depth + 1)) {
                return false;
            }

            continue;
        }

        // Everything else has to be an access *of this address*, on `walkAddress`'s rule: an
        // instruction naming the pointer anywhere but an address operand is the address reaching
        // somewhere this walk cannot follow it, and a call is one of those.
        auto used = user->used();
        auto self = address - base;

        LowerAccess accesses[kMaxAccesses];
        auto count = lowerInstAccesses(user, accesses);
        auto named = Size(0);

        for(Size which = 0; which < count; which++) {
            if(*accesses[which].address == self) named++;
        }

        auto positions = Size(0);
        for(Size i = 0; i < used.length; i++) {
            if(used.ptr[i] == self) positions++;
        }

        if(named == 0 || named != positions) return false;
    }

    return true;
}

/*
 * The other end of the same waste: a copy *out of* a phi, into a slot with nothing else in it.
 *
 * `tryForward` below moves a value's writes forward into the storage it was going to be copied to.
 * That rewrite needs the writes and the copy in one block, and an aggregate that arrives through a
 * **block argument** never has them there:
 *
 *      b_21 { %22 = alloca 20; ..writes..; jmp b_19 }
 *      b_7  { %23 = alloca 20; ..writes..; jmp b_19 }
 *      b_19 { %20 = phi [b_21, %22], [b_7, %23]; %24 = alloca 20; copy %24, %20, 20; ..reads.. }
 *
 * Two slots is what the shape is *for* - each arm builds its own value and the edge picks one - and
 * `%24` is a third holding a copy of whichever arrived. Nothing needs it: the phi is already an
 * address, the bytes behind it are already the bytes `%24` would hold, and the readers below can
 * read them where they are. So the rewrite is the plain one - every use of `%24` becomes a use of
 * `%20`, and the copy and the slot go away - and it is worth twenty bytes moved and a frame object
 * per site.
 *
 * The shape is `Try(Maybe(a), a, ()).toOutcome` inlined, and every `?`, `?.` and `match` arm that
 * produces an aggregate: `test/bench/findings.md` §106.4 is where it was counted, at 71 of the 406
 * whole-slot copies over `test/resolve`.
 *
 * ## What it has to prove
 *
 * Four things, and none of them is `tryForward`'s aliasing question - because nothing moves. The
 * writes stay exactly where they are; what changes is which address the *readers* use, and they read
 * the same bytes either way.
 *
 *  - **the copy moves the whole of both slots.** The count is the destination's own byte count and
 *    every incoming allocation's, so what the readers find is everything the copy would have put
 *    there. A partial copy leaves the rest of the destination to be found.
 *  - **the phi answers this copy and nothing else.** A second reader of it would keep reading the
 *    predecessor's slot while the readers of `%24` now write through it, and the two would be one
 *    piece of storage with two owners.
 *  - **each incoming slot is dead below its edge** - `tiedToBlock`. Every access to it is inside the
 *    block that hands it over, so nothing reads it afterwards and nothing writes it again. This is
 *    what makes the destination's later writes - a teardown call, a tag store - land in storage
 *    nobody else is looking at.
 *  - **and the copy's block cannot reach any of those blocks.** A loop would fill an incoming slot a
 *    second time, below the readers now reading it: the deadness above is a statement about one trip
 *    through the edge, and this is what makes one trip all there is.
 *
 * The destination's own uses are asked one thing, and it is the same statement from its side:
 * nothing may read it *before* the copy, since before the copy it holds whatever the last value in
 * that frame object left. A use above the copy in its own block, or in any block that can reach it,
 * is one of those.
 */
bool tryAdoptPhi(LowerBase base, LowerFunction& fun, LowerBlock& block, HashMap<U32, U32>& position,
                 HashMap<U32, U32>& reaching, const DominatorTree& dominators, BlockEdits& edits,
                 LowerInstCopy& copy) {
    auto source = base[copy.from];
    auto destination = base[copy.to];
    if(source == destination) return false;

    auto phi = source->inst();
    if(phi->kind != LowerInst::Phi || phi->block != &block - base) return false;
    if(source->uses.size() != 1) return false;

    auto slot = destination->inst();
    if(slot->kind != LowerInst::Alloca) return false;

    auto size = constantOf(base, ((LowerInstAlloca*)slot)->byteCount);
    auto count = constantOf(base, copy.count);
    if(size.isNothing() || count.isNothing()) return false;
    if(size.unwrap() == 0 || size.unwrap() != count.unwrap()) return false;

    auto here = position.getValue(U32((LowerInst*)&copy - base));
    if(!here) return false;

    /*
     * Every reader of the destination, asked two things.
     *
     * **The phi has to reach it**, which is the plain SSA obligation and the one this rewrite would
     * otherwise break: the destination is a frame object and is therefore readable from anywhere,
     * while the phi is a value defined at the top of one block. A reader in a sibling arm is
     * dominated by neither, and pointing it at the phi is a use of something that is not live there.
     * `LowerValue::dominates` is the question; the same-block case is asked as a position instead,
     * because a phi dominates its own block's instructions including the ones above the copy - and
     * one of *those* is a read of the slot before anything filled it.
     *
     * **And it must not run again before the copy does.** A reader whose block can reach this one is
     * one that does, on the second trip round; the deadness proved of each arm below is a statement
     * about one trip through the edge, and this is the same statement from the destination's side.
     */
    blocksReaching(base, block, reaching);

    for(auto userPtr: destination->uses.contents(base)) {
        auto user = base[userPtr];
        if(user == (LowerInst*)&copy || user == slot) continue;

        if(reaching.getValue(U32(user->block))) return false;

        if(user->block == &block - base) {
            auto at = position.getValue(U32(user - base));
            if(!at || at.unwrap() <= here.unwrap()) return false;
            continue;
        }

        if(!source->dominates(base, user, dominators)) return false;
    }

    // Every arm's slot, held so that the alignment is raised only once the whole shape has been
    // proved - a refusal below must leave the function exactly as it found it.
    SmallArray<LowerInstAlloca*, 4> arms;
    auto used = phi->used();
    auto sources = ((LowerInstPhi*)phi)->sources();

    if(used.length == 0 || used.length != sources.length) return false;

    for(Size i = 0; i < used.length; i++) {
        auto incoming = base[used.ptr[i]];
        auto definition = incoming->inst();
        if(definition->kind != LowerInst::Alloca) return false;

        auto held = constantOf(base, ((LowerInstAlloca*)definition)->byteCount);
        if(held.isNothing() || held.unwrap() != size.unwrap()) return false;

        if(!tiedToBlock(base, incoming, phi, sources.ptr[i], 0)) return false;

        // And the edge is taken once. `blocksReaching` is asked of the arm rather than of this
        // block, which is the direction it walks, so the map is rebuilt once per arm.
        blocksReaching(base, *base[sources.ptr[i]], reaching);
        if(reaching.getValue(U32(&block - base))) return false;

        arms.push((LowerInstAlloca*)definition);
    }

    /*
     * The rewrite. The readers move to the phi, and the slot they were reading has nothing left in
     * it - which is the same two lines `tryForward` ends with, in the other direction.
     */
    // Each arm now holds what the destination held, so it has to be aligned as the destination was.
    for(auto arm: arms) arm->alignment = max(arm->alignment, ((LowerInstAlloca*)slot)->alignment);

    detach(base, (LowerInst*)&copy);
    block.instructions.remove(base, Size(here.unwrap()));

    replaceUses(base, fun.module->arena, copy.to, copy.from);
    edits.retire(base, slot);
    edits.touched = Size(here.unwrap());
    edits.kept = false;
    return true;
}

/*
 * One copy, and whether the value it moves could have been built where it was going.
 *
 * Everything the header lists is checked here, in the order that makes the cheap refusals cheap: the
 * shape of the copy, then the temporary's own uses, then the stretch between the first of them and
 * the copy. The last is the only part that walks instructions the copy does not name.
 */
bool tryForward(LowerBase base, LowerFunction& fun, LowerBlock& block, HashMap<U32, U32>& position,
                HashMap<U32, U32>& reaching, LowerPtr<LowerValue> returnPlace, BlockEdits& edits,
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
        if(!touchesStorage(inst)) continue;

        // A call the temporary is handed to is one of the writes being moved rather than something
        // that could reach the destination - which is what the check above has just established for
        // every one of them.
        if(inst->kind == LowerInst::Call && handedTo.containsValue(inst)) continue;

        // §27 And the destination's own definition is not in this stretch once the rewrite has run,
        // since lifting it above `first` is the rewrite. It is the only call that leaves this way.
        if(movesAllocator && inst == definition) continue;

        LowerAccess accesses[kMaxAccesses];
        auto count = lowerInstAccesses(inst, accesses);

        auto known = false;
        for(Size which = 0; which < count; which++) {
            auto owner = allocationBase(base, *accesses[which].address);
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
    edits.retire(base, allocation);
    edits.touched = first;
    edits.kept = false;

    // The destination now holds what the temporary held, so it has to be aligned as the temporary
    // was. Only an allocation can be told so; a pointer from outside already satisfies whatever its
    // own storage promised, which is what let the copy write there at all.
    if(destination->inst()->kind == LowerInst::Alloca) {
        auto target = (LowerInstAlloca*)destination->inst();
        target->alignment = max(target->alignment, ((LowerInstAlloca*)allocation)->alignment);
    }

    return true;
}

/*
 * `collectContainedAllocations` for `tryRelay`, taken when a relay first gets as far as placing its
 * accesses and again after any `tryForward` or `tryAdoptPhi` of the function - J163. It used to pass
 * `placeOf` an empty table, which is safe and never tells a contained source apart from a pointer a
 * parameter or a call holds. Not once per function: `tryForward` redirects a temporary's uses - a call
 * it was handed to among them - to the destination, which can make an allocation that was contained
 * stop being so. A relay does not (J166, `forwardCopies`), and rebuilding after each of those was a
 * walk of the whole function per relay.
 */
struct ContainedTable {
    HashMap<U32, U8> table;
    bool fresh = false;

    HashMap<U32, U8>& get(LowerBase base, LowerFunction& fun) {
        if(!fresh) {
            table.clear();
            collectContainedAllocations(base, fun, table);
            fresh = true;
        }

        return table;
    }
};

/*
 * Whether a write to `write` cannot reach a byte of `read` - J160.
 *
 * `mayOverlap` (lower_store.h), and one fact it does not state: storage outside this frame - a place
 * rooted at a parameter or a global - is never an allocation of it. A pointer the caller passed
 * existed before this call's frame did, so it cannot point into a slot the frame allocates, whether
 * or not that slot's address has since been handed to a callee.
 */
bool placesApart(LowerBase base, const LowerPlace& write, const LowerPlace& read) {
    if(!mayOverlap(write, read)) return true;
    if(!write.root || !read.root || write.root == read.root) return false;

    auto outside = [&](const LowerPlace& place) {
        auto inst = place.root->inst();
        return inst && (inst->kind == LowerInst::Arg || inst->kind == LowerInst::Global);
    };

    return (write.allocation && outside(read)) || (read.allocation && outside(write));
}

/*
 * A temporary that only relays - J160: a copy fills the whole of it from somewhere and a later copy in
 * the same block moves the whole of it on, and nothing else names it.
 *
 *      copy %t, %a, 60                      ..
 *      ..                            ->     copy %b, %a, 60
 *      copy %b, %t, 60
 *
 * `tryForward` is the same shape asked from the other end - build the value where the second copy
 * was going to put it - and it has to move the first copy's write down to where the destination is
 * known to be unobserved, so it needs the destination defined above the first copy and every access
 * between to resolve to an allocation. The token an `iter fn` hands over fails both: the destination
 * is the record's hand-over slot, `self + 152`, computed after the callee filled the source, and the
 * stretch stores the token's two positions into `self`. This rewrite moves nothing but the *source*
 * of the second copy, so all it has to prove is that nothing in between writes the source's bytes.
 *
 * What it is worth, measured (J160): in the shipping stage, whose lowering relays a value through a
 * temporary at nearly every place it moves one, `test/base`'s `.text` went from 906,520 bytes to
 * 848,536 (-6.4%); in the new stage, which removes most of them before this tier, 942,408 to
 * 940,936. On `test/bench/lexer`'s `iter` it took one of the two 60-byte block copies of every
 * handed-over token away - 222 to 200 cycles a token - and `splitAggregateSlots`' word cut of a wide
 * unnamed cell (lower_split.cpp) is what took the rest: a block copy out of storage a callee has
 * just written a field at a time is a load the store buffer cannot forward, however many there are.
 *
 * Refused where anything in the stretch might write the source: a call, an access this pass cannot
 * place, a write `placesApart` cannot separate from it. And where the destination might overlap the
 * source, which the two copies kept apart and one would not.
 */
bool tryRelay(LowerBase base, LowerFunction& fun, LowerBlock& block, HashMap<U32, U32>& position,
              ContainedTable& contained, BlockEdits& edits, LowerInstCopy& copy) {
    auto temporary = base[copy.from];
    auto allocation = temporary->inst();
    if(!allocation || allocation->kind != LowerInst::Alloca || copy.to == copy.from) return false;

    auto size = constantOf(base, ((LowerInstAlloca*)allocation)->byteCount);
    auto count = constantOf(base, copy.count);
    if(size.isNothing() || count.isNothing()) return false;
    if(size.unwrap() == 0 || size.unwrap() != count.unwrap()) return false;

    // Two uses: this copy's source, and the source-less end of one copy that writes all of it.
    LowerInstCopy* filler = nullptr;
    Size uses = 0;

    for(auto userPtr: temporary->uses.contents(base)) {
        auto user = base[userPtr];
        uses++;
        if(user == (LowerInst*)&copy) continue;
        if(user->kind != LowerInst::Copy || filler) return false;

        auto other = (LowerInstCopy*)user;
        if(other->to != copy.from || other->from == copy.from) return false;

        auto filled = constantOf(base, other->count);
        if(filled.isNothing() || filled.unwrap() != size.unwrap()) return false;

        filler = other;
    }

    if(uses != 2 || !filler || filler->block != &block - base) return false;

    auto here = position.getValue(U32((LowerInst*)&copy - base));
    auto there = position.getValue(U32((LowerInst*)filler - base));
    if(!here || !there || there.unwrap() >= here.unwrap()) return false;

    auto& table = contained.get(base, fun);
    auto source = placeOf(base, table, filler->from, size.unwrap());
    auto destination = placeOf(base, table, copy.to, size.unwrap());
    if(!placesApart(base, destination, source)) return false;

    for(auto i = Size(there.unwrap()) + 1; i < Size(here.unwrap()); i++) {
        auto inst = base[block.instructions.get(base, i)];
        if(hasLowerTrait(inst, kLowerOrdered) || isVolatileMemoryAccess(inst)) return false;
        if(!hasLowerTrait(inst, kLowerWrites)) continue;

        LowerAccess accesses[kMaxAccesses];
        auto written = lowerInstAccesses(inst, accesses);
        if(written == 0 || inst->kind == LowerInst::Call) return false;

        // A copy writes its first access and reads its second - see `gatherLoopWrites`.
        if(inst->kind == LowerInst::Copy) written = 1;

        for(Size a = 0; a < written; a++) {
            auto place = placeOf(base, table, *accesses[a].address, placeExtent(base, accesses[a]));
            if(!placesApart(base, place, source)) return false;
        }
    }

    // The filler goes, the copy reads what the filler read, and the temporary has no use left.
    auto from = filler->from;
    eraseInst(base, (LowerInst*)filler);
    replaceUses(base, fun.module->arena, temporary - base, from);
    edits.retire(base, allocation);
    edits.touched = Size(there.unwrap());
    edits.kept = true;
    return true;
}

// The two rewrites over one function, with the dominator tree `tryAdoptPhi` reads already built or
// already known to be unnecessary - see `forwardCopyDestinations`, which is the split's whole reason.
void forwardCopies(LowerBase base, LowerFunction& fun, LowerPtr<LowerValue> returnPlace,
                   const DominatorTree& dominators) {
    HashMap<U32, U32> position;
    HashMap<U32, U32> reaching;
    ContainedTable contained;
    BlockEdits edits;

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        edits.block = blockPtr;

        /*
         * From the bottom up, so that a chain of temporaries collapses without needing a walk per
         * link: forwarding the copy nearest the end gives the copy above it the destination the
         * whole chain was heading for, and that copy is the next one this reaches.
         *
         * After a rewrite the scan carries on from the copy it rewrote - asked again where the copy
         * is still there, since a relay can leave it a copy `tryForward` now takes - and a pass that
         * rewrote anything is followed by another over the whole block, which is what catches a
         * copy below that the rewrite made possible. J166: this used to restart the block at every
         * rewrite, renumbering all of it each time, and the shipping stage's lowering relays a value
         * at nearly every move - a 44,547-instruction block in `test/base` was renumbered 113 M
         * entries' worth, which was a third of that stage's whole native compile. The two orders
         * reach the same fixpoint on every program the suite builds, byte for byte.
         */
        auto changed = true;
        while(changed) {
            changed = false;
            indexBlock(base, *block, position);

            for(auto i = block->instructions.size(); i-- > 0;) {
                auto inst = base[block->instructions.get(base, i)];
                if(inst->kind != LowerInst::Copy) continue;

                auto& moved = *(LowerInstCopy*)inst;

                // The phi case first, because it is the cheaper question and the two are disjoint:
                // `tryForward` needs the copy's source to be an allocation this block writes, and
                // this one needs it to be a block argument.
                auto rewrote = tryAdoptPhi(base, fun, *block, position, reaching, dominators, edits, moved) ||
                               tryForward(base, fun, *block, position, reaching, returnPlace, edits, moved);

                // A relay moves nothing but the second copy's source, which leaves every allocation
                // as contained as it was - so it is the one rewrite that keeps `contained` fresh.
                if(rewrote) {
                    contained.fresh = false;
                } else if(tryRelay(base, fun, *block, position, contained, edits, moved)) {
                    rewrote = true;
                } else {
                    continue;
                }

                changed = true;

                // Renumber what the rewrite moved - see `BlockEdits` - and go on from the copy.
                auto end = min(i + 1, block->instructions.size());
                for(auto at = edits.touched; at < end; at++) {
                    position.add(U32(block->instructions.get(base, at)), U32(at));
                }

                i = edits.kept ? Size(position.getValue(U32(inst - base)).unwrap()) + 1 : min(i, end);
            }
        }

        edits.flush(base);
    }
}

}

/*
 * The dominator tree is built only for a function that has a copy out of a block argument in it,
 * which is what the scan below decides - `tryAdoptPhi` is its only reader, and the shape is a
 * handful of sites in a program. Two calls rather than an assignment because a `DominatorTree` holds
 * `SmallArray`s and is therefore built where it is named; neither rewrite touches an edge, so one
 * build lasts the whole run.
 */
void forwardCopyDestinations(LowerBase base, LowerFunction& fun, LowerPtr<LowerValue> returnPlace) {
    auto wanted = false;

    for(auto blockPtr: fun.blocks.contents(base)) {
        for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
            auto inst = base[instPtr];
            if(inst->kind != LowerInst::Copy) continue;
            if(!isPhi(base[((LowerInstCopy*)inst)->from]->inst())) continue;

            wanted = true;
            break;
        }

        if(wanted) break;
    }

    if(!wanted) {
        DominatorTree none;
        forwardCopies(base, fun, returnPlace, none);
        return;
    }

    auto dominators = fun.buildDominatorTree(base);
    forwardCopies(base, fun, returnPlace, dominators);
}
