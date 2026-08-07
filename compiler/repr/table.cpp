#include "table.h"
#include "Net/Buffer.h"

U32 tableLayout(Buffer<const TableSlot> slots, PackOffsets& offsets) {
    offsets.clear();
    for(Size i = 0; i < slots.length; i++) offsets.push(tableSlotOffset(U16(i)));

    return tableSize(U16(slots.length));
}

U32 tableMetricValue(ReprTable& repr, const TableSlot& slot) {
    auto type = slot.metricType();
    if(!type) return 0;

    auto& of = repr.of(type);

    switch(slot.metric) {
        case TypeMetricKind::Align: return of.align;
        case TypeMetricKind::Stride: return of.stride;
        case TypeMetricKind::Size: break;
    }

    return of.size;
}

/*
 * What one word cell holds, for whichever backend is about to write it down.
 *
 * The one place the three word kinds are turned into a number, so that a native table and a JS one
 * cannot disagree about what a PackedMetric combines to. An address cell has no word and answers
 * zero - what fills it in is a relocation, or on JS the emitted name.
 */
U32 tableWordValue(ReprTable& repr, const TableSlot& slot) {
    switch(slot.kind) {
        case TableCell::Int: return slot.value();
        case TableCell::Metric: return tableMetricValue(repr, slot);
        case TableCell::PackedMetric:
            return (tableMetricValue(repr, slot) << kPackedMetricShift) | slot.extra;
        case TableCell::Function:
        case TableCell::Global: break;
    }

    return 0;
}

void writeTableWords(ReprTable& repr, Buffer<const TableSlot> slots, Buffer<const U32> offsets,
                     ByteBuffer bytes) {
    Net::BufferWriter writer(bytes.ptr, bytes.length);

    for(Size i = 0; i < slots.length; i++) {
        // An address is left as zeroes; what fills it in is a relocation the caller records.
        if(isAddressCell(slots[i].kind)) continue;

        // A measurement is answered here rather than emitted: what is in the slot is which
        // measurement of which type, and the type handle it names never reaches the output. See
        // TableSlot.
        auto word = tableWordValue(repr, slots[i]);

        writer.offset(offsets[i]);

        if(repr.target.byteOrder == LittleEndian) {
            writer.writeInt<LittleEndian>(word);
        } else {
            writer.writeInt<BigEndian>(word);
        }
    }
}
