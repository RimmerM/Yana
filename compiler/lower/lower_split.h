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
 * Dropping a cell is sound when nothing can observe its bytes, and there are exactly two ways to
 * observe them: an access that reads the cell, and a copy that carries it *out* to an address this
 * pass cannot see - a parameter, a heap block, a pointer a call returned, a slot it declined to split -
 * where anyone may read them. So an access marks its cell live, a copy out marks live every cell it
 * carries that something *wrote*, liveness travels across every copy between slots, and a live cell
 * is given storage and moved like any other. A cell nothing wrote holds unspecified bytes wherever it
 * goes, and leaving a destination's own unspecified bytes in their place is no difference at all.
 *
 * Where the bytes *came from* does not enter. A copy in from outside carries bytes that mean whatever
 * the sender meant, but a cell nothing reads and nothing hands on is never asked what they were. That
 * used to be a third rule - a copy in made its cells live - and it was the expensive one: it kept a
 * value's padding alive through every temporary it passed once the value had been read out of a heap
 * block, so `Maybe(Size)`'s seven bytes above the tag were copied three times per `exchange`, as
 * overlapping moves that stalled store forwarding (Analysis-External-Iterators.md's 2026-09-15
 * switch-cost note).
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
 * nothing computes from any more - to `removeDeadValues` at the end of the run.
 */
void splitAggregateSlots(LowerBase base, LowerFunction& fun);
