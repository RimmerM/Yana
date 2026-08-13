#pragma once

#include "lower.h"

/*
 * Aggregates taken apart into one slot per field.
 *
 * `promoteStackSlots` holds a slot in a register when the whole of its traffic is a load or a store
 * of the whole thing. That describes an integer local and a scalar record, and it describes nothing
 * with two fields in it: the moment an address has arithmetic done to it the slot is memory, and
 * every `Option`, every iterator result and every teardown outcome is exactly that shape. So a
 * function whose values are aggregates keeps all of them on the stack, however small the pieces are.
 *
 * `VecString.indexOfVectors` was the standing demonstration - 1,370 bytes against `llc -Os`'s 189,
 * and almost none of the difference SIMD. What it holds is a nested `Option`, built four levels deep
 * by four inlined bodies, and what that reaches the backend as is nine allocations of 16, 24 and 32
 * bytes, a `store` of a tag into each, and eleven whole-record `copy`s between them.
 *
 * This pass is what stands between the two. It splits a slot into the pieces its own accesses name,
 * gives each piece storage of its own, and rewrites every copy of the whole as a copy per piece -
 * and then promotion, unchanged, holds each piece in a register and phis it at the joins. Nothing
 * here promotes anything or knows what a register is; what it does is turn one shape promotion
 * cannot see into several it can.
 *
 * ## The partition, and why it is a fixpoint
 *
 * A slot's cut points are the offsets its own accesses begin and end at - a `store` of four bytes at
 * zero cuts at 0 and 4 - and the ends of every `copy` that names it. That is not enough on its own,
 * because a copy carries the *other* slot's structure into this one: `Just(v)` writes a tag and a
 * payload into a 16-byte temporary and then copies the whole of it into the payload of a 24-byte
 * one, and the second slot never names those two fields at all. So a cut point on either end of a
 * copy is a cut point on the other, mapped through the two offsets, and the partition is the least
 * fixpoint of that rule over the whole graph of slots and copies between them.
 *
 * A slot whose every access then falls inside exactly one cell is split. One whose access straddles
 * a cell - a load of a word over two fields that were written separately, which is what a `@bits`
 * pair reaches this stage as - is not, and dropping it out changes what its copy partners may do, so
 * the partition is recomputed and the whole thing settles rather than being decided once.
 *
 * ## The cells that carry nothing, and the one rule that keeps them safe
 *
 * A record has padding, and after the partition the padding is a cell nothing in the program reads
 * or writes: `Option(Int)` is a four-byte tag at zero and an eight-byte payload at eight, and the
 * four bytes between them are named by no access anywhere. Those cells are given no storage and are
 * left out of every copy, which is where most of the win is - a 24-byte copy becomes two moves of
 * twelve bytes between registers rather than three of eight between stack slots.
 *
 * That is only sound while the bytes are this function's own uninitialized frame memory, and one
 * thing can make them something else: a copy *into* a split slot from an address this pass cannot
 * see - a parameter, a heap block, a pointer a call returned - carries in bytes that mean whatever
 * the sender meant by them. So a copy from outside marks every cell it lands on live, and a live
 * cell is given storage and moved like any other, whether or not anything reads it.
 *
 * With that rule the induction closes: a dead cell's content comes from nowhere but another dead
 * cell or from storage nothing ever wrote, so a copy *out* of a split slot may leave the
 * corresponding bytes of its destination alone. What it leaves there is the destination's own
 * unspecified bytes instead of ours, and the program had no way to tell the two apart.
 *
 * ## A move is a load and a store, not a copy
 *
 * A per-cell copy would be correct and would buy nothing: promotion reads the register type of a
 * slot off the loads and stores that name it, so a cell whose only traffic is copies has no type and
 * stays in memory - which is every payload in a chain that merely passes a value along. So a cell of
 * a width a register holds is moved as a `load` and a `store` rather than as a `copy`, and the type
 * they carry is the one the cell's accesses agree on anywhere in the graph, propagated across the
 * copies beside the cut points. A cell nothing types and no register holds is moved as a copy, which
 * is the case this pass has nothing to say about and leaves as it found it.
 *
 * ## Order
 *
 * Between `forwardCopyDestinations` and `promoteStackSlots`, and it depends on both. Forwarding
 * removes the whole-allocation copies that have a temporary on one end, which is strictly less for
 * this pass to split; promotion is what makes the split worth anything at all, since every cell this
 * produces is exactly the shape it was already looking for.
 *
 * Like both of them it leaves its litter - the byte count of an allocation that is gone, an offset
 * nothing computes from any more - to `removeDeadValues` and `removeDeadConstants` at the end of the
 * run.
 */
void splitAggregateSlots(LowerBase base, LowerFunction& fun);
