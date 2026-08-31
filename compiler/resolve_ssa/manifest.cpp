#include "manifest.h"

namespace ssa {

/*
 * The target, read off the settings in one place.
 *
 * Every architecture this compiler emits for is little-endian, so today this is a constant wearing a
 * switch. It is written as the switch anyway, for the reason `ReprTarget::byteOrder` is a field: the
 * cost of naming the assumption is one line, and the cost of not naming it is that the first
 * big-endian target has to find every site that wrote a number into bytes.
 */
NativeTarget nativeTargetFor(const CompileSettings& settings) {
    NativeTarget target;

    switch(settings.arch) {
        case TargetArch::X64:
        case TargetArch::X86:
        case TargetArch::ARM:
        case TargetArch::ARM64:
            target.byteOrder = LittleEndian;
            break;
    }

    return target;
}

static constexpr U32 kWholeRecord = ~U32(0);

/*
 * One record's layout, in one walk.
 *
 * The size, the alignment and every field's offset come out of the same accumulation, because they
 * are one answer: a size that was computed separately from the offsets is a size that can disagree
 * with them, and the disagreement is a write past the end of an allocation. `stop` is how a caller
 * asks for a prefix - `fieldOffsetNative` stops at the field it wants and reads the running offset.
 *
 * A prefix walk is the field count for one field and its square for all of them, and it stays that
 * way: the offsets of a record are the sizes and alignments of its members, so the walk is the one
 * arithmetic that produces them and a table of offsets would be a second answer to keep in step. The
 * term that *grew* is the one the memo above removes - each member's own manifestation, which for a
 * nested record was the whole layout again at every level.
 */
static U32 layoutRecord(DeclBase base, mir::RecordType& record, U32 stop, U32& align) {
    // The whole-record answer, memoized on the record - `RecordType::nativeAlign`. A prefix is not
    // one and neither reads nor fills it.
    if(stop == kWholeRecord && record.nativeAlign) {
        align = record.nativeAlign;
        return record.nativeSize;
    }

    U32 offset = 0;
    align = 1;

    U32 index = 0;
    for(auto field: record.fields.contents(base)) {
        // The field's own contract, not `owned`: an external field holds a reference to somebody
        // else's object and is a pointer, and asking for the owned answer would lay out the object.
        auto member = manifestNative(base, mir::Occurrence { field.occurrence.type, field.occurrence.contract, nullptr });
        auto memberAlign = member.align ? member.align : 1;

        offset = (offset + memberAlign - 1) & ~(memberAlign - 1);
        if(index == stop) return offset;

        offset += member.size;
        if(memberAlign > align) align = memberAlign;
        index++;
    }

    // The tail padding, so that an array of these strides correctly and a second record holding one
    // starts its next field where the first one ends.
    auto size = (offset + align - 1) & ~(align - 1);

    record.nativeSize = size;
    record.nativeAlign = U16(align);
    return size;
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

bool arrivesAsAddress(DeclBase base, const mir::Occurrence& occurrence) {
    return manifestNative(base, occurrence).inMemory || isReferenceValue(base, occurrence);
}

Manifestation manifestNative(DeclBase base, const mir::Occurrence& occurrence) {
    auto& logical = *base[occurrence.type];

    /*
     * A non-owning occurrence of a scalar is the scalar, and an interior shape is not physical yet.
     *
     * True for every type this milestone has and *not* true in general - a shared `String` is a
     * window where an owned one is an owner. Both halves are asserted rather than ignored, so the
     * first occurrence this table cannot answer for stops here instead of being answered wrongly.
     *
     * The shape half is a *count* rather than emptiness, and the difference is what `Reader(a')`
     * makes. A shape an occurrence carries is currently only ever the type's own schema renumbered -
     * the same positions, holding the same things, under a different signature's origin names - so
     * it changes no layout and this table is right to key on the logical type. What *would* change
     * one is a shape the type does not have, which is `Maybe(a'T)` against `Maybe(T)` and arrives
     * with generic application; comparing the position counts is what tells the two apart.
     */
    auto schema = mir::schemaOf(base, occurrence.type);

    /*
     * A **function value** is the one type whose interior position is not a field.
     *
     * The pair holds a code word and an environment word whether or not the environment is somebody
     * else's storage, so the closure shape (`mir::Shapes::closure`) names a position that is already
     * there and changes no layout - which is precisely the property this check is about. `schemaOf`
     * has nothing to answer for a structural type, so the count is compared against the one position
     * a `Fun` has rather than against a declaration that does not exist.
     */
    if(logical.kind == mir::Type::Fun) {
        assertTrue(mir::Shapes::isEmpty(occurrence.shape) ||
                   base[occurrence.shape]->slots.size() == 1);
    } else {
        assertTrue(mir::Shapes::isEmpty(occurrence.shape) ||
                   (schema && base[occurrence.shape]->slots.size() == base[schema]->slots.size()));
    }

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
        // **Not** `inMemory`. The pointer is the value: installing one stores a word rather than
        // copying the object, and the field of a record that holds a loan is one word wide. What
        // makes a *load* of one no instruction is that the value is the address it names, which is
        // the contract's own answer and is asked where the load is lowered.
        return Manifestation { LowerType(LowerLane::Pointer, 0, false), kPointerSize, kPointerAlign,
                               false };
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
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), kPointerSize,
                                   kPointerAlign, false };
        /*
         * A function value - two words, `{code, env}`, and address-only like a record.
         *
         * Address-only because the lower IR has no pair, and a pair is what this is: the code word
         * alone would be a function *pointer*, which is a different thing that cannot carry
         * captures. What a call does with the two is read them back out, which is exactly what it
         * does with a record's fields.
         *
         * The size is stated in `manifest.h` and nowhere else, and stated as *two words* rather
         * than as a number of bytes: both ends of the layout - the construction in `lower_fun.cpp`
         * and the call that reads it back - go through `kFunCodeOffset`/`kFunEnvOffset`, and all
         * three derive from `kPointerSize`.
         */
        case mir::Type::Fun:
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), kFunSize, kPointerAlign,
                                   true };

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
            auto size = layoutRecord(base, (mir::RecordType&)logical, kWholeRecord, align);
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), size, align, true };
        }
        /*
         * A type parameter - Analysis-IR.md §11.4's G5, and the row `manifest.cpp` was written
         * around.
         *
         * **Address-only, with a size that is not a number.** A body compiled once has no
         * substitution to turn `a` into a layout, so every value of one is reached through storage
         * and the storage is sized from the descriptor slot the caller filled in. That is a
         * manifestation like any other - the same `inMemory` an owned record sets, for the same
         * reason - and `dynamic` is the one thing about it a consumer may not ignore.
         *
         * Not reachable from a *specialized* body: every type there is bound, so a variable arriving
         * here would be a body converted under the wrong substitution. `checkErasable` is what holds
         * the other side of that - a form is only chosen erased for a body whose every type is
         * either concrete or exactly one of these.
         */
        case mir::Type::Variable:
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), 0, 0, true, true };

        /*
         * A number written where a type is, which no value has - `mir::ConstType`.
         *
         * It reaches this table only as an *argument* of one, and an argument is not a member: a
         * `Buf(4)` manifests over its fields, and `4` is not one of them. Empty rather than a size
         * of zero, on the row above's reasoning - what has no representation should fail the
         * `verifyBody` that asks for one rather than lower to nothing.
         */
        case mir::Type::Const:
        case mir::Type::KindCount:
            break;
    }

    return Manifestation {};
}

/*
 * How many bits of its storage a value of this type actually owns, or zero where the question does
 * not apply.
 *
 * `Bool` owns one bit of the byte it is given, which is what makes it the narrow case that exists
 * *now*: nothing here co-packs yet, so its shift is zero at every site, and the encoding is what has
 * to be right rather than the number. An integer owns its whole storage by construction -
 * `manifestNative` gives it `bits / 8` bytes - so `@bits(n)`, which is the other half of the
 * library's use of this, is what makes one of them answer differently.
 */
U32 narrowBitsOf(DeclBase base, DeclPtr<mir::Type> type) {
    auto& logical = *base[type];
    if(logical.kind == mir::Type::Bool) return 1;
    if(logical.kind == mir::Type::Int) return ((mir::IntType&)logical).bits;
    return 0;
}

// Which of the three shapes a requirement promising this result type puts its slot in - see
// `PropertyForm`. The one place the decision is made, because it is made at both ends.
PropertyForm propertyFormOf(DeclBase base, DeclPtr<mir::Type> result) {
    if(!result || !mir::isConcreteType(base, result)) return PropertyForm::Witness;

    auto bits = narrowBitsOf(base, result);
    if(!bits) return PropertyForm::Offset;

    return bits < manifestNative(base, mir::owned(result)).size * 8 ? PropertyForm::Narrow
                                                                   : PropertyForm::Offset;
}

} // namespace ssa
