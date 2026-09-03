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
    target.integers = targetIntWidths(settings);

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

/*
 * A written integer type's width on this machine.
 *
 * The switch `intrinsic::TargetWidth` exists to defer, and the one place it is answered - so that
 * "how wide is a `Size`" has a single definition rather than one per site that lays a value out.
 * A fixed width answers with what it states, which is why every caller may ask this rather than
 * testing which kind of width it has.
 */
U16 intBitsOn(NativeTarget target, const mir::IntType& type) {
    switch(type.target) {
        case intrinsic::TargetWidth::Word:     return target.integers.word;
        case intrinsic::TargetWidth::CodeUnit: return target.integers.codeUnit;
        case intrinsic::TargetWidth::Fixed:    break;
    }

    return type.bits;
}

/*
 * How many bytes an integer of this many bits is stored in - the width rounded up to one a machine
 * addresses and aligns.
 *
 * Not `bits / 8`, and `WideInt` is why: fifty-three bits are not a whole number of bytes of
 * anything, so a size taken by division is six - an alignment that is not a power of two, and a
 * record laid out on it is one nothing can address. Every width the language has fits in a word, so
 * the ladder stops at eight.
 */
static U32 storageBytesFor(U16 bits) {
    if(bits <= 8) return 1;
    if(bits <= 16) return 2;
    if(bits <= 32) return 4;
    return 8;
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
static U32 layoutRecord(Layout layout, mir::RecordType& record, U32 stop, U32& align) {
    auto base = layout.base;

    // The whole-record answer, memoized on the record - `RecordType::nativeAlign`. A prefix is not
    // one and neither reads nor fills it.
    if(stop == kWholeRecord && record.nativeAlign) {
        align = record.nativeAlign;
        return record.nativeSize;
    }

    /*
     * The discriminant, which is the record's first bytes where it has one.
     *
     * At offset zero and before every payload, which is a decision rather than an observation: what
     * reads a tag is a match, and a match on a value whose payload it has not proved anything about
     * may only touch bytes every arm agrees are the tag. Putting it first makes that the same
     * address for every arm and for every instantiation, so the load is one instruction with no
     * layout question in front of it.
     */
    auto tagBits = mir::tagBitsOf(base, record);
    auto tagBytes = U32(tagBits / 8);

    U32 offset = tagBytes;
    align = tagBytes ? tagBytes : 1;

    /*
     * The payload, laid out **once per constructor over the same storage**.
     *
     * That is what makes a sum a sum: the arms are alternatives, so their fields start at one
     * payload offset and the record is as large as the widest of them. `first`/`count` is the range
     * this walk is over, and a field index identifies its own constructor - which is why there is no
     * downcast step in a place path and no second projection to keep in step with this arithmetic.
     *
     * The payload offset is decided *after* every arm has been measured, because it depends on the
     * strongest alignment any of them needs and the arms are measured in one pass. So each arm is
     * laid out from zero here and the offset is added at the end, which is also what lets the prefix
     * answer come out of the same walk.
     */
    U32 payload = 0;
    U32 payloadAlign = 1;
    U32 answer = 0;
    auto found = false;

    for(auto constructor: record.cons.contents(base)) {
        U32 within = 0;

        for(U16 i = constructor.first; i < U16(constructor.first + constructor.count); i++) {
            auto field = record.fields.get(base, i);

            // The field's own contract, not `owned`: an external field holds a reference to somebody
            // else's object and is a pointer, and asking for the owned answer would lay out the
            // object.
            auto member = manifestNative(layout, mir::Occurrence {
                field.occurrence.type, field.occurrence.contract, nullptr });
            auto memberAlign = member.align ? member.align : 1;

            within = (within + memberAlign - 1) & ~(memberAlign - 1);

            if(i == stop) {
                answer = within;
                found = true;
            }

            within += member.size;
            if(memberAlign > payloadAlign) payloadAlign = memberAlign;
        }

        if(within > payload) payload = within;
    }

    offset = (offset + payloadAlign - 1) & ~(payloadAlign - 1);
    if(payloadAlign > align) align = payloadAlign;

    // A prefix asks for one field's offset, which is where its arm starts plus where it sits inside
    // it. A `stop` no field matched is a caller asking about a record with no such field, which
    // `fieldOffsetNative` has already asserted against.
    if(stop != kWholeRecord) return found ? offset + answer : offset;

    // The tail padding, so that an array of these strides correctly and a second record holding one
    // starts its next field where the first one ends.
    auto size = (offset + payload + align - 1) & ~(align - 1);

    record.nativeSize = size;
    record.nativeAlign = U16(align);
    return size;
}

U32 fieldOffsetNative(Layout layout, DeclPtr<mir::Type> record, U16 field) {
    auto declared = (mir::RecordType*)layout.base[record];
    assertTrue(declared->kind == mir::Type::Record);
    assertTrue(field < declared->fields.size());

    U32 align = 1;
    return layoutRecord(layout, *declared, field, align);
}

U32 elementStrideNative(Layout layout, DeclPtr<mir::Type> element) {
    // Owned, whatever the container's own contract is: what an element costs is a property of the
    // element type, and an array reached through a borrow holds the same bytes as one this frame
    // owns. The `Array` row in `manifestNative` reaches `strideOf` with the manifestation it already
    // has, so the rounding is written once and done once.
    return strideOf(manifestNative(layout, mir::owned(element)));
}

U32 discriminantOffsetNative(Layout layout, DeclPtr<mir::Type> record) {
    auto declared = (mir::RecordType*)layout.base[record];
    assertTrue(declared->kind == mir::Type::Record);
    assertTrue(mir::tagBitsOf(layout.base, *declared));

    // First, so that every arm agrees where it is without having proved which arm it is - see
    // `layoutRecord`, where the placement is made and the reason is written out.
    return 0;
}

bool isAddressOnly(Layout layout, const mir::Occurrence& occurrence) {
    return manifestNative(layout, occurrence).inMemory;
}

bool isReferenceValue(Layout layout, const mir::Occurrence& occurrence) {
    auto base = layout.base;
    auto contract = occurrence.contract;
    if(contract.isOwned()) return false;

    // The two the source published - see `manifestNative`, which reads this function rather than
    // restating the pair.
    if(contract.access() == mir::Access::Unique || contract.origin() == mir::Origin::Slot) {
        return true;
    }

    // Otherwise it is a reference exactly when the object is one: a shared loan of a record is that
    // record's address, and a shared loan of a scalar is the scalar.
    return manifestNative(layout, mir::owned(occurrence.type)).inMemory;
}

bool arrivesAsAddress(Layout layout, const mir::Occurrence& occurrence) {
    return manifestNative(layout, occurrence).inMemory || isReferenceValue(layout, occurrence);
}

Manifestation manifestNative(Layout layout, const mir::Occurrence& occurrence) {
    auto base = layout.base;
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

    /*
     * A **sequence window** - `[T]` in a binding position, and the one row where the contract's
     * access form decides the whole answer.
     *
     * Two words, `{items, length}`, whatever the logical type says an owner holds. That is the third
     * layer doing its job: `Array(T)` owned is a growable array whose fields are the library's, and
     * a shared or unique window into one is a base and an extent - one logical type, two contracts,
     * two representations, and nothing above this file allowed to know either.
     *
     * Address-only, like a function value and for the same reason: the lower IR has no pair, so a
     * value of two words is reached through storage and what a use does with it is read the words
     * back out.
     *
     * Above the switch because it is a fact about the *contract*, exactly as the reference-value row
     * below it is. **Which** logical types may carry the form is `resolve_mir`'s to police, and
     * deliberately not asked here: this table takes a type, a contract and a target, and reaching
     * for the role table to find out whether a record is *the* array would be a fourth input. What
     * is checked is the kind, which is the mistake that could actually arrive - a form set on a
     * scalar would otherwise fall through into a register row and lose a word.
     */
    if(occurrence.contract.form() == mir::AccessForm::SequenceWindow) {
        assertTrue(logical.kind == mir::Type::Record || logical.kind == mir::Type::Array);
        return Manifestation { LowerType(LowerLane::Pointer, 0, false), kWindowSize, kPointerAlign,
                               false, true };
    }

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
    if(isReferenceValue(layout, occurrence)) {
        // **Not** `inMemory`. The pointer is the value: installing one stores a word rather than
        // copying the object, and the field of a record that holds a loan is one word wide. What
        // makes a *load* of one no instruction is that the value is the address it names, which is
        // the contract's own answer and is asked where the load is lowered.
        return Manifestation { LowerType(LowerLane::Pointer, 0, false), kPointerSize, kPointerAlign,
                               false, false };
    }

    switch(logical.kind) {
        case mir::Type::Unit:
            // No bytes and no register. A unit result is a function that returns nothing, which is
            // what the lower IR's empty return type list says.
            return Manifestation { LowerType(LowerLane::Int32, 0, false), 0, 1, false, false };
        case mir::Type::Int: {
            // The machine's answer, which for `Size`, `USize` and `CodeUnit` is the whole of what
            // they are - this is the bottom of the layer they exist for, and the only place a
            // `TargetWidth` becomes a number.
            auto bits = intBitsOn(layout.target, (mir::IntType&)logical);
            auto lane = bits > 32 ? LowerLane::Int64 : LowerLane::Int32;
            auto bytes = storageBytesFor(bits);

            // The register is 32 or 64 bits and the *storage* is the width rounded up to one a
            // machine addresses: a narrower scalar arrives in a 32-bit register and states its width
            // on the access that reads it, which is the rule every pass below the lower IR is
            // written against. `narrowBitsOf` is the other half - how many of those bytes' bits the
            // value actually owns, which for `WideInt` is fifty-three of sixty-four.
            return Manifestation { LowerType(lane, 0, false), bytes, bytes,
                                   ((mir::IntType&)logical).isSigned, false };
        }
        case mir::Type::Float: {
            auto bits = ((mir::FloatType&)logical).bits;
            auto lane = bits == 32 ? LowerLane::Float32 : LowerLane::Float64;
            return Manifestation { LowerType(lane, 0, false), U32(bits / 8), U32(bits / 8), false,
                                   false };
        }
        case mir::Type::Pointer:
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), kPointerSize,
                                   kPointerAlign, false, false };
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
                                   false, true };

        /*
         * A fixed array - `n` elements at the element's **stride**, and address-only.
         *
         * The stride and not the size, and that is the whole of this row: a record with trailing
         * padding is nine bytes and sixteen apart, because §10.3 gives a record's trailing padding
         * to whoever contains it - and two *elements* of an array are exactly the case where nobody
         * may land in it. `[Pad *3]` is forty-eight and not forty-one, and an array laid out at the
         * size would have its second element overlap its first's tail.
         *
         * A **quantified** count has no size here. It only reaches this table inside a body compiled
         * once, where every other layout is the descriptor's too - `dynamic` is that answer, and it
         * is the same one a bare type parameter gives.
         *
         * The element is manifested **owned**, whatever the array's own contract is: what an element
         * costs is a property of the element type, and an array reached through a shared borrow holds
         * the same bytes as one this frame owns.
         */
        case mir::Type::Array: {
            auto& array = (mir::ArrayType&)logical;
            auto element = manifestNative(layout, mir::owned(array.element));

            if(!array.hasFixedLength() || element.dynamic) {
                return Manifestation { LowerType(LowerLane::Pointer, 0, false), 0, 0, false, true,
                                       true };
            }

            auto align = element.align ? element.align : 1;

            return Manifestation { LowerType(LowerLane::Pointer, 0, false),
                                   U32(strideOf(element) * array.length), align, false, true };
        }

        case mir::Type::Record: {
            auto& record = (mir::RecordType&)logical;

            /*
             * An **enum-layout** record is its discriminant and nothing else, so it is a register
             * value - `RecordLayout::Enum`, and the one refinement no kind can make.
             *
             * The same statement the old stage's `isDirectType` makes as "a fact about the instance
             * rather than about `Record`", and it is a *decision* rather than an observation: the
             * arms carry nothing, so a value of one has no bytes beyond the tag and reaching it
             * through storage would be an address where the language has a small integer. Reading
             * the tag out of one is still a projection through the record's own storage, which every
             * local has, so `match` needs nothing from this row.
             */
            if(record.layout == mir::RecordLayout::Enum) {
                auto range = mir::tagRangeOf(base, record);
                auto lane = range.bits > 32 ? LowerLane::Int64 : LowerLane::Int32;
                auto bytes = storageBytesFor(range.bits);

                return Manifestation { LowerType(lane, 0, false), bytes, bytes, range.isSigned,
                                       false };
            }

            /*
             * Every other record is reached through storage, and its `LowerType` is therefore the
             * pointer that carries it - the shared IR has no aggregate type, so a value of one is an
             * address everywhere below this line.
             *
             * `inMemory` is what says so, and it is the field every consumer reads rather than
             * testing the kind: an address-only *scalar* - a value too wide for a register, a
             * generic parameter of unknown size - is the same answer for a different reason, and a
             * consumer that asked "is it a record" would have to learn each of them.
             */
            U32 align = 1;
            auto size = layoutRecord(layout, record, kWholeRecord, align);
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), size, align, false,
                                   true };
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
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), 0, 0, false, true,
                                   true };

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
 * An **enum-layout record** owns as many bits as its constructor numbers need, which for `Bool` is
 * one bit of the byte it is given - the narrow case that exists *now*. Nothing here co-packs yet, so
 * its shift is zero at every site, and the encoding is what has to be right rather than the number.
 *
 * Most integers own their whole storage, because `manifestNative` rounds their width up to a byte
 * count and they land on one. **`WideInt` is the first that does not** - fifty-three bits in eight
 * bytes - and it is why the two are separate functions rather than one: what has to be reserved for
 * a value and how much of it the value means are different questions, and they first give different
 * answers here without any attribute being written. `@bits(n)` is the other half of the library's
 * use of this and answers here the same way: `@bits(13) U32` is thirteen bits reserved in two.
 */
U32 narrowBitsOf(Layout layout, DeclPtr<mir::Type> type) {
    auto& logical = *layout.base[type];
    if(logical.kind == mir::Type::Int) return intBitsOn(layout.target, (mir::IntType&)logical);

    if(logical.kind == mir::Type::Record) {
        auto& record = (mir::RecordType&)logical;
        if(record.layout != mir::RecordLayout::Enum) return 0;

        // The numbers, not the byte they are stored in - `tagRangeOf` rounds its width up to a
        // storage one and this is the question that wants the count. A pinned negative takes the
        // whole of it, since a sign bit is not a pattern anything narrower holds.
        auto range = mir::tagRangeOf(layout.base, record);
        if(range.isSigned) return range.bits;

        U32 bits = 1;
        while(bits < 64 && (U64(1) << bits) <= U64(range.highest)) bits++;
        return bits;
    }

    return 0;
}

// Which of the three shapes a requirement promising this result type puts its slot in - see
// `PropertyForm`. The one place the decision is made, because it is made at both ends.
PropertyForm propertyFormOf(Layout layout, DeclPtr<mir::Type> result) {
    if(!result || !mir::isConcreteType(layout.base, result)) return PropertyForm::Witness;

    auto bits = narrowBitsOf(layout, result);
    if(!bits) return PropertyForm::Offset;

    return bits < manifestNative(layout, mir::owned(result)).size * 8 ? PropertyForm::Narrow
                                                                     : PropertyForm::Offset;
}

} // namespace ssa
