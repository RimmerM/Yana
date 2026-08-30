#include "manifest.h"

namespace ssa {

/*
 * One record's layout, in one walk.
 *
 * The size, the alignment and every field's offset come out of the same accumulation, because they
 * are one answer: a size that was computed separately from the offsets is a size that can disagree
 * with them, and the disagreement is a write past the end of an allocation. `stop` is how a caller
 * asks for a prefix - `fieldOffsetNative` stops at the field it wants and reads the running offset.
 */
static U32 layoutRecord(DeclBase base, mir::RecordType& record, U32 stop, U32& align) {
    U32 offset = 0;
    align = 1;

    U32 index = 0;
    for(auto field: record.fields.contents(base)) {
        // The field's own contract, not `owned`: an external field holds a reference to somebody
        // else's object and is a pointer, and asking for the owned answer would lay out the object.
        auto member = manifestNative(base, mir::Occurrence { field.type, field.contract, nullptr });
        auto memberAlign = member.align ? member.align : 1;

        offset = (offset + memberAlign - 1) & ~(memberAlign - 1);
        if(index == stop) return offset;

        offset += member.size;
        if(memberAlign > align) align = memberAlign;
        index++;
    }

    // The tail padding, so that an array of these strides correctly and a second record holding one
    // starts its next field where the first one ends.
    return (offset + align - 1) & ~(align - 1);
}

U32 fieldOffsetNative(DeclBase base, DeclPtr<mir::Type> record, U16 field) {
    auto declared = (mir::RecordType*)base[record];
    assertTrue(declared->kind == mir::Type::Record);
    assertTrue(field < declared->fields.size());

    U32 align = 1;
    return layoutRecord(base, *declared, field, align);
}

bool isAddressOnly(DeclBase base, const mir::Occurrence& occurrence) {
    return manifestNative(base, occurrence).inMemory;
}

bool isReferenceValue(DeclBase base, const mir::Occurrence& occurrence) {
    auto contract = occurrence.contract;
    if(contract.isOwned()) return false;

    // The two the source published - see `manifestNative`, which reads this function rather than
    // restating the pair.
    if(contract.access() == mir::Access::Unique || contract.origin() == mir::Origin::Slot) {
        return true;
    }

    // Otherwise it is a reference exactly when the object is one: a shared loan of a record is that
    // record's address, and a shared loan of a scalar is the scalar.
    return manifestNative(base, mir::owned(occurrence.type)).inMemory;
}

Manifestation manifestNative(DeclBase base, const mir::Occurrence& occurrence) {
    auto& logical = *base[occurrence.type];

    /*
     * A non-owning occurrence of a scalar is the scalar, and a scalar has nothing borrowed inside it.
     *
     * True for every type this milestone has and *not* true in general - a shared `String` is a
     * window where an owned one is an owner, and a record with an interior slot is a different shape
     * from the same record without one. Both halves are asserted rather than ignored, so the first
     * occurrence this table cannot answer for stops here instead of being answered wrongly.
     */
    assertTrue(mir::Shapes::isEmpty(occurrence.shape));
    assertTrue(occurrence.contract.form() == mir::AccessForm::WholeValue);

    /*
     * A loan the source *published* is reached through storage, whatever the logical type is.
     *
     * Two occurrences ask for that and `doc/spec/ownership.md` names both. Exclusive access is one:
     * "a mutable borrow's whole contract is writing through to the same storage", and a `&x: I32`
     * that arrived in a register would have nowhere to write back to. An external-origin qualifier
     * is the other: `x: source'Int` "deliberately borrows caller-owned storage and can root a
     * `source'Int` result", so "its manifestation must now retain an address".
     *
     * What is deliberately *not* here is a plain shared loan of a trivially copyable type. That one
     * is a register value, because the spec copies at that position rather than borrowing - a
     * trivial value's loan and its copy are physically the same thing, and `readMode` never produces
     * a loan there for this table to have to represent.
     *
     * Above the switch rather than as a case in it, because it is a fact about the *contract* and
     * the same answer for every logical type - which is the layer's whole shape: `manifest(logical
     * type, contract, target)`, and this is the row where the second argument decides alone.
     */
    if(isReferenceValue(base, occurrence)) {
        // **Not** `inMemory`. The pointer is the value: installing one stores eight bytes rather
        // than copying the object, and the field of a record that holds a loan is eight bytes wide.
        // What makes a *load* of one no instruction is that the value is the address it names, which
        // is the contract's own answer and is asked where the load is lowered.
        return Manifestation { LowerType(LowerLane::Pointer, 0, false), 8, 8, false };
    }

    switch(logical.kind) {
        case mir::Type::Unit:
            // No bytes and no register. A unit result is a function that returns nothing, which is
            // what the lower IR's empty return type list says.
            return Manifestation { LowerType(LowerLane::Int32, 0, false), 0, 1, false };
        case mir::Type::Bool:
            // One byte of storage in a 32-bit register, holding 0 or 1. The width is the *storage*
            // answer and nothing above this file has it - which is the point of the layer: `Bool`
            // is two values up there, and `if` reads one rather than testing a bit pattern.
            return Manifestation { LowerType(LowerLane::Int32, 0, false), 1, 1, false };
        case mir::Type::Int: {
            auto bits = ((mir::IntType&)logical).bits;
            auto lane = bits > 32 ? LowerLane::Int64 : LowerLane::Int32;

            // The register is 32 or 64 bits and the *storage* is the declared width: a narrower
            // scalar arrives in a 32-bit register and states its width on the access that reads it,
            // which is the rule every pass below the lower IR is written against.
            return Manifestation { LowerType(lane, 0, false), U32(bits / 8), U32(bits / 8), false };
        }
        case mir::Type::Float: {
            auto bits = ((mir::FloatType&)logical).bits;
            auto lane = bits == 32 ? LowerLane::Float32 : LowerLane::Float64;
            return Manifestation { LowerType(lane, 0, false), U32(bits / 8), U32(bits / 8), false };
        }
        case mir::Type::Pointer:
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), 8, 8, false };
        case mir::Type::Record: {
            /*
             * A record is reached through storage, and its `LowerType` is therefore the pointer that
             * carries it - the shared IR has no aggregate type, so a value of one is an address
             * everywhere below this line.
             *
             * `inMemory` is what says so, and it is the field every consumer reads rather than
             * testing the kind: an address-only *scalar* - a value too wide for a register, a
             * generic parameter of unknown size - is the same answer for a different reason, and a
             * consumer that asked "is it a record" would have to learn each of them.
             */
            U32 align = 1;
            auto size = layoutRecord(base, (mir::RecordType&)logical, ~U32(0), align);
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), size, align, true };
        }
        case mir::Type::KindCount:
            break;
    }

    return Manifestation {};
}

} // namespace ssa
