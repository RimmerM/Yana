#include "manifest.h"

namespace ssa {

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
        case mir::Type::KindCount:
            break;
    }

    return Manifestation {};
}

} // namespace ssa
