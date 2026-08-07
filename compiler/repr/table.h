#pragma once

#include "repr.h"
#include "../resolve/module.h"

/*
 * Where the slots of a compiler-built table land in memory.
 *
 * **Every cell is four bytes, so slot N is at 4N.** That is the whole rule, and it is the same rule
 * on every target - which is why almost nothing is left in this header. A word is a 32-bit number; an
 * address is a 32-bit offset from the image anchor, not a pointer (see TableCell). So there is no
 * padding, no alignment question, and nothing here depends on the target at all.
 *
 * It did not used to be so. An address cell was the target's pointer width, which made a table both
 * twice the size and target-shaped: `tableSlotOffset` took a ReprTarget, four 32-bit words rounded up
 * to the same sixteen bytes as three, and a witness table - which is nothing but addresses - was half
 * padding. Narrowing the address cell removed the only thing that varied.
 *
 * Two kinds of caller, and the difference is what shaped the interface before it collapsed:
 *
 *  - whoever *writes* a table has the slots in hand and walks them, which is materializeTable;
 *  - whoever *reads* one has no table at all. A generic body loading `size` out of a descriptor it
 *    was handed knows the shape and not the instance, since the instance is a runtime pointer.
 *
 * A witness table is read by code that was compiled against these positions and cannot be recompiled
 * when a better arrangement is found, which makes it the one layout in the compiler that is a
 * contract rather than a decision. There is deliberately no packing, no reordering and no niche in it.
 *
 * ## An address is an offset from the image anchor
 *
 * A slot holds `target - &anchor` as a signed 32-bit number, where the anchor is one label per
 * program - see Program::imageAnchor. A reader takes the anchor's address and adds: one `lea` per
 * function, hoisted and shared, then one `add` per slot.
 *
 * Anchor-relative rather than absolute, and rather than relative to the slot itself. Both of the
 * alternatives were tried:
 *
 *  - **Absolute** would be correct for an ELF image, whose load address `elf.h` deliberately fixes
 *    low enough to fit in 32 bits - and silently wrong for the JIT, which maps wherever `mmap`
 *    chooses. It also needs a load address, so nothing could be written until the image was placed.
 *  - **Self-relative** (`target - &slot`) is position-independent and needs no anchor, and it works
 *    for every table that is a constant in the image. It cannot work for a `GenEnv`, which a generic
 *    function calling another one builds *on the frame* - see genEnvironment - because a frame is
 *    tens of terabytes from the image and the difference does not fit. The callee reads its
 *    environment one way whichever kind it was handed, so one form has to serve both.
 *
 * What the anchor buys over self-relative is that a stack-built table measures from the same place a
 * constant one does. What it costs is the `lea`, which is pure, loop-invariant and free after the
 * first. Both are position-independent: the anchor's address is itself RIP-relative.
 *
 * `target - &anchor` is known as soon as the image is assembled, so table slots are written by
 * `resolveRelocations` and never reach `applyDataRelocations`. The bytes are final before the image
 * is placed, and identical bytes serve an ELF image at a fixed 0x400000 and a JIT buffer anywhere.
 *
 * The one constraint this adds: the image must be one contiguous region under 2GB, which it is -
 * code then data in a single buffer, and the JIT copies that buffer. A target that split them would
 * need the anchor per section, or pointers back.
 */

// What one cell occupies, and therefore what a slot index is worth in bytes. One constant because
// there is only one kind of cell as far as the layout is concerned - see isAddressCell for the one
// thing that still distinguishes them, which is whether a relocation writes it.
static constexpr U32 kTableCellSize = 4;

// Where slot `slot` begins, and how big a table with `slotCount` slots is. Neither takes a target:
// the layout is the same everywhere, which is the point.
inline U32 tableSlotOffset(U16 slot) { return U32(slot) * kTableCellSize; }
inline U32 tableSize(U16 slotCount) { return U32(slotCount) * kTableCellSize; }

/*
 * Where each slot of this table goes, and how big it comes out.
 *
 * `offsets` comes back with one entry per slot, which is what turns a Function or Global cell into a
 * relocation at the right place. Those cells are left as zeroes in the bytes: until every symbol is
 * placed there is no offset to write.
 */
U32 tableLayout(Buffer<const TableSlot> slots, PackOffsets& offsets);

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
    auto size = tableLayout(slots, offsets);

    ByteBuffer bytes((Byte*)arena.alloc(size), size);
    set(bytes.ptr, size, 0);

    writeTableWords(repr, slots, toBuffer(offsets), bytes);
    return bytes;
}

// What a Metric cell measures, for a backend materializing one itself. Null-safe in the way the rest
// of the Repr table is: a cell naming no type answers zero rather than asking about nothing.
U32 tableMetricValue(ReprTable& repr, const TableSlot& slot);

// What any word cell holds - the one place Int, Metric and PackedMetric become a number, shared so
// that the two backends cannot combine one differently. Zero for an address cell.
U32 tableWordValue(ReprTable& repr, const TableSlot& slot);
