#pragma once

#include "repr.h"
#include "../resolve/module.h"

/*
 * Where the slots of a compiler-built table land in memory.
 *
 * This is the *native* materialization of the tables resolve/witness.h describes, and it is here
 * rather than there for the same reason everything else in this directory is: a slot has a position
 * only once some target has said how wide an address is and what it must be aligned to. A target
 * that reads its tables another way - JS reads them as arrays, where a slot's position is its index
 * and nothing has a width - never includes this header. It is not describing the same numbers
 * differently; it has no offsets to describe.
 *
 * Two kinds of caller, and the difference is what shapes the interface:
 *
 *  - whoever *writes* a table has the slots in hand and walks them, which is materializeTable;
 *  - whoever *reads* one has no table at all. A generic body loading `size` out of a descriptor it
 *    was handed knows the shape and not the instance, since the instance is a runtime pointer. So a
 *    position has to be computable from the shape alone.
 *
 * Every table this compiler builds is some 32-bit words followed by some addresses - a descriptor's
 * five numbers and three lifecycle slots, a witness's three counts and its three sections of
 * pointers, an environment's schema word and its slots. So "the shape" is one number, the count of
 * leading words, and each numbering in witness.h states its own as kWordCount.
 *
 * The rule itself is the obvious one and lives in exactly one place, TableLayout::place: a word is
 * four bytes, an address is the target's, and each sits at the next multiple of its own width. There
 * is deliberately no packing, no reordering and no niche in it. A witness table is read by code that
 * was compiled against these positions and cannot be recompiled when a better arrangement is found,
 * which makes it the one layout in the compiler that is a contract rather than a decision.
 */

// How wide one cell is. The only thing a layout needs to know about a slot; see isAddressCell.
enum class CellWidth: U8 { Word, Address };

struct TableLayout {
    explicit TableLayout(const ReprTarget& target): target(target) {}

    // The offset of the next cell, which is then consumed. Called once per slot, in order.
    U32 place(CellWidth width);

    // The whole table, padded so that an array of them would keep every element aligned.
    U32 size() const;

    const ReprTarget& target;
    U32 offset = 0;
    U32 align = 1;
};

// Where slot `slot` of a table shaped as `wordCount` words followed by addresses begins, and how
// big such a table with `slotCount` slots is. Both take the target rather than reading a global one,
// because what is being emitted for is the whole question.
U32 tableSlotOffset(const ReprTarget& target, U16 wordCount, U16 slot);
U32 tableSize(const ReprTarget& target, U16 wordCount, U16 slotCount);

/*
 * Where each slot of this table goes, and how big it comes out.
 *
 * `offsets` comes back with one entry per slot, which is what turns a Function or Global cell into a
 * relocation at the right place. Those cells are left as zeroes in the bytes: until the module is
 * placed there is no address to write.
 */
U32 tableLayout(const ReprTarget& target, Buffer<const TableSlot> slots, PackOffsets& offsets);

/*
 * The word cells, into storage the caller allocated and zeroed.
 *
 * Takes the whole table rather than only the target because a Metric cell is a question - "how wide
 * is this type" - and answering it is exactly what a ReprTable is for. That is the point of the cell
 * existing: the number is produced here, by the backend that is about to emit, instead of having
 * been decided when the descriptor was built.
 *
 * Words go through a writer at the target's byte order rather than being copied out of a host U32:
 * what reads them is the target, and copying the host's bytes is right only for as long as the two
 * agree.
 */
void writeTableWords(ReprTable& repr, Buffer<const TableSlot> slots, Buffer<const U32> offsets,
                     ByteBuffer bytes);

// The two together. Templated only because the two module arenas are different types; there is one
// materialization and this is it.
template<class Arena>
ByteBuffer materializeTable(Arena& arena, ReprTable& repr, Buffer<const TableSlot> slots,
                            PackOffsets& offsets) {
    auto size = tableLayout(repr.target, slots, offsets);

    ByteBuffer bytes((Byte*)arena.alloc(size), size);
    set(bytes.ptr, size, 0);

    writeTableWords(repr, slots, toBuffer(offsets), bytes);
    return bytes;
}

// What a Metric cell measures, for a backend materializing one itself. Null-safe in the way the rest
// of the Repr table is: a cell naming no type answers zero rather than asking about nothing.
U32 tableMetricValue(ReprTable& repr, const TableSlot& slot);
