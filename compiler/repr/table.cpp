#include "table.h"
#include "Net/Buffer.h"

U32 TableLayout::place(CellWidth width) {
    auto bytes = width == CellWidth::Address ? target.pointerSize : U32(sizeof(U32));
    auto cellAlign = width == CellWidth::Address ? target.pointerAlign : U32(sizeof(U32));

    if(cellAlign > align) align = cellAlign;

    auto at = (offset + cellAlign - 1) & ~(cellAlign - 1);
    offset = at + bytes;
    return at;
}

U32 TableLayout::size() const {
    return (offset + align - 1) & ~(align - 1);
}

U32 tableSlotOffset(const ReprTarget& target, U16 wordCount, U16 slot) {
    TableLayout layout(target);
    U32 at = 0;

    for(U16 i = 0; i <= slot; i++) {
        at = layout.place(i < wordCount ? CellWidth::Word : CellWidth::Address);
    }

    return at;
}

U32 tableSize(const ReprTarget& target, U16 wordCount, U16 slotCount) {
    TableLayout layout(target);
    for(U16 i = 0; i < slotCount; i++) {
        layout.place(i < wordCount ? CellWidth::Word : CellWidth::Address);
    }

    return layout.size();
}

U32 tableLayout(const ReprTarget& target, Buffer<const TableSlot> slots, PackOffsets& offsets) {
    offsets.clear();

    TableLayout layout(target);
    for(Size i = 0; i < slots.length; i++) {
        offsets.push(layout.place(isAddressCell(slots[i].kind) ? CellWidth::Address : CellWidth::Word));
    }

    return layout.size();
}

U32 tableMetricValue(ReprTable& repr, const TableSlot& slot) {
    auto type = TypePtr(slot.value);
    if(!type) return 0;

    auto& of = repr.of(type);

    switch(slot.metric) {
        case TypeMetricKind::Align: return of.align;
        case TypeMetricKind::Stride: return of.stride;
        case TypeMetricKind::Size: break;
    }

    return of.size;
}

void writeTableWords(ReprTable& repr, Buffer<const TableSlot> slots, Buffer<const U32> offsets,
                     ByteBuffer bytes) {
    Net::BufferWriter writer(bytes.ptr, bytes.length);

    for(Size i = 0; i < slots.length; i++) {
        // An address is left as zeroes; what fills it in is a relocation the caller records.
        if(isAddressCell(slots[i].kind)) continue;

        auto word = slots[i].kind == TableCell::Metric
            ? tableMetricValue(repr, slots[i]) : slots[i].value;

        writer.offset(offsets[i]);

        if(repr.target.byteOrder == LittleEndian) {
            writer.writeInt<LittleEndian>(word);
        } else {
            writer.writeInt<BigEndian>(word);
        }
    }
}
