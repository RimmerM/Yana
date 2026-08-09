#pragma once

#include "lower.h"

/*
 * Values built in a temporary and then copied where they were wanted.
 *
 * A record is constructed by allocating storage for it, writing its fields, and copying the result
 * into whatever asked for one. That is the only shape resolve's places have - a place is an address,
 * and a nested construction is therefore an address per level - so `String { data: StringData { bytes:
 * Array(U8) { run: Run(U8) {..}, .. } } }` reaches this stage as four allocations and four copies of
 * the same sixteen bytes, ending in one into the caller's return slot:
 *
 *      %4 = alloca 12                       store  %0, %bytes, 8
 *      store %4, %bytes, 8                  %8 = add %0, 8
 *      %8 = add %4, 8                       store  %8, %14, 4
 *      store %8, %14, 4              ->     %15 = add %0, 12
 *      copy  %3, %4, 12                     store  %15, %7, 4
 *      %15 = add %3, 12                     ret
 *      store %15, %7, 4
 *      copy  %2, %3, 16
 *      %16 = alloca 16
 *      copy  %16, %2, 16
 *      copy  %0, %16, 16                    (%0 is the caller's return slot)
 *      ret
 *
 * Nothing here is a fact about records. What the transform knows is that a `copy` moves the whole of
 * an allocation into somewhere else, and that an allocation whose entire content is about to be
 * copied somewhere could have been written there in the first place - so every use of the temporary
 * becomes a use of the destination and both the temporary and the copy go away. Return-slot
 * forwarding, scalar replacement of a copied aggregate and the removal of an intermediate temporary
 * are the same rewrite applied at three places, which is why there is one rule rather than three.
 *
 * ## Why this is the tier for it
 *
 * `opt_scalar.cpp` takes the same shape apart in the resolve IR and stops where ownership begins: a
 * field-wise write relocates each field exactly as a whole-value write relocated all of them, but
 * only where relocating is bytes rather than a call, so anything owning a resource - which every
 * container and every string does - is left whole. That test is right there and cannot be weakened,
 * because a local it split would still have its teardown.
 *
 * Here there is no ownership left to reason about. A `copy` is bytes, an `alloca` is bytes, and a
 * temporary that stops existing takes no drop with it because drops became calls several stages ago.
 * The rewrite is therefore about *storage* and asks nothing about types at all.
 *
 * ## What it has to prove
 *
 * Four things, and each rules out a specific way of being wrong:
 *
 *  - **the copy moves the whole allocation.** The count is the allocation's own byte count, so what
 *    reaches the destination is everything the temporary ever held. A partial copy would leave the
 *    bytes it did not carry to be found in the destination.
 *  - **nothing but the copy has the temporary's address.** Every use is a load, a store, a copy, a
 *    fill, or a constant offset used for one of those - so the address reaches no call, is stored
 *    nowhere, and survives nothing. This is what makes redirecting every use of it *all* of them.
 *  - **the copy is the last of those uses, and they are all in its own block.** The rewrite moves
 *    each write forward to where the copy was, so the stretch it moves over has to be straight-line
 *    code. A use after the copy would be a read of a temporary that no longer exists.
 *  - **nothing in that stretch can see the destination.** This is the aliasing question, and it is
 *    answered rather than assumed: every memory access between the first write and the copy has to
 *    resolve to an allocation, and to one that is not the allocation the destination is part of.
 *    Two allocations are disjoint by construction, so most of this needs no analysis at all - and a
 *    destination that is a parameter or a global needs none either, since neither can be storage
 *    this frame created. The one case that does is a destination whose origin is this function's own
 *    - a pointer a call returned - where an allocation whose address escaped could be it.
 *
 * The last one is why a call in that stretch stops the rewrite: a call may touch anything, and the
 * destination is exactly the thing it must not touch.
 *
 * ## §7.4 The temporary a call fills
 *
 * The four above describe a temporary this function *writes*. A record filled by a call is the same
 * shape with the writes on the other side of a call boundary - `newStringOfCapacity` is handed
 * somewhere to build a string, and the result is copied into the local that wanted it:
 *
 *      %2 = alloca 16                       %3 = alloca 16
 *      call newStringOfCapacity, %2, 37     call newStringOfCapacity, %3, 37
 *      %3 = alloca 16                ->     call pushString, %3, ..
 *      copy %3, %2, 16                      ..
 *      call pushString, %3, ..
 *
 * The call is as redirectable as a store: it writes through the pointer it is given, and giving it
 * the destination writes the same bytes in the same place. So a call is admitted as a use of the
 * temporary, and the ordinary rewrite - every use of the temporary becomes a use of the destination -
 * covers it with nothing added.
 *
 * What is added is one obligation, and it is what the aliasing argument above becomes when the
 * writes are a callee's. The destination stops being written *at* the copy and starts being written
 * *during* the call, so a callee that could reach the destination by some other route would see it
 * half-written where before it saw it untouched. Nothing can be proved about what a callee does, so
 * what is proved instead is what it can name: the destination has to be storage of this frame whose
 * address has reached nothing but plain accesses by the time the call runs.
 *
 * "By the time" is the whole of it. The destination in `Text.line` is handed to four *later* calls
 * and is still invisible to the first one, because an address passed on below a call cannot be an
 * address that call already had. So what each use is asked is **can this run before the call**, and
 * a position is only one of the two orderings a function has:
 *
 *  - a use in the call's own block runs before it exactly when it is written above it, *and* the
 *    block is not one that reaches itself. A block inside a loop does, so a use written below the
 *    call there is a use written above it on the next time round;
 *  - a use anywhere else runs before the call exactly when its block can reach the call's, which is
 *    what `blocksReaching` answers for every block at once. `Text.showSigned` reads its destination
 *    in five later blocks and none of them leads back to the one that builds it.
 *
 * Nothing downstream of a use is asked separately: a use of a value is reachable from that value's
 * definition, so a block that cannot reach the call has no successor that can. A destination that is
 * a parameter or a global is refused outright: those are the two the argument above needs nothing
 * for, and the two this one can have nothing about.
 *
 * ## §27 The destination a call produced, and the one reordering this pass performs
 *
 * `Tree.build` is the shape above with the destination on the far side of the writes as well:
 *
 *      %10 = alloca 20                      %11 = call allocateHeap, 20
 *      call build, %10, %8, %9        ->    call build, %11, %8, %9
 *      %11 = call allocateHeap, 20
 *      copy %11, %10, 20
 *
 * The destination has to exist where the first write is, since that is where it starts being written,
 * and here it does not. An `alloca` in that position is simply *moved* above, which costs nothing: a
 * fixed frame object exists for the whole of a function wherever the instruction naming its address
 * was written. A call cannot be moved on that argument, and this one is the only call this pass will
 * move at all.
 *
 * **The allocator is the one callee a pass at this tier may know something about**, and it knows it
 * because the compiler wrote the call: storage the escape analysis placed on the heap is lowered to a
 * call to `Native.allocateHeap` and to nothing else, so `LowerModule::allocator` names it exactly.
 * What the move exchanges is the order of that call and the calls the temporary is handed to - and
 * nothing else, because the stretch between them is already required to hold nothing but accesses
 * that resolve to allocations of this frame.
 *
 * So the whole of the argument is about those two calls, and it is three statements:
 *
 *  - **a fresh block is fresh whenever it is taken.** The allocator cuts one out of the bump area or
 *    takes it off a free list, and either way nothing live holds a pointer into it. Taking it earlier
 *    cannot collide with anything the intervening call allocates, because that call's own allocations
 *    are then taken after this one rather than before it.
 *  - **which address comes back is not something a program may depend on.** The intervening call
 *    allocates and frees as it pleases, so the two orders return different addresses; that is the
 *    entire observable difference, and the placement that produced the call was the compiler's own
 *    decision rather than anything written in the program.
 *  - **an allocation taken and not used is a leak and not a fault.** If the intervening call does not
 *    return - an abort, an exit - the block is one the program had not taken before. Nothing reads it
 *    and nothing frees it, which on a path that is ending is not a difference.
 *
 * What is *not* claimed is the reverse: a heap block is only distinct from another while both are
 * live, so one is admitted as the destination and never as the `owner` of an unrelated access. Two
 * `alloca`s are distinct storage for the whole of a frame and two heap blocks are not.
 *
 * The obligation §7.4 states is unchanged and is what the aliasing half rests on - `addressHiddenBefore`
 * is asked about the block the same way it is asked about a frame slot, and the answer is easier: a
 * pointer the allocator has just returned has reached nothing at all.
 *
 * ## Order
 *
 * Before `promoteStackSlots`, and the direction of the dependency is that way round: forwarding
 * removes whole-allocation copies, and a copy of a promotable slot is one of the shapes promotion
 * has to reproduce as a load or a store. Removing it first leaves promotion strictly less to do. The
 * reverse order would work and buy nothing.
 *
 * Each block is walked from the bottom up, which is what makes a chain collapse in one pass rather
 * than in one pass per link: the copy nearest the end names the destination the whole chain is
 * heading for, so forwarding it hands that destination to the copy above, and so on up. Taken
 * downwards, each step would only forward into the next temporary.
 *
 * Like promotion, it leaves its litter to `removeDeadConstants` in lower_fold.h - the byte count of
 * an allocation that no longer exists is an immediate nothing reads.
 */
void forwardCopyDestinations(LowerBase base, LowerFunction& fun);
