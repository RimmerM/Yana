#include "manifest.h"

namespace ssa {

U32 fieldOffsetNative(Layout layout, DeclPtr<mir::Type> record, U16 field,
                      DeclPtr<mir::OwnershipSchema> shape) {
    auto declared = (mir::RecordType*)layout.base[record];
    assertTrue(declared->kind == mir::Type::Record);
    assertTrue(field < declared->fields.size());

    // Read out of the record's own `Repr` rather than walked again here - `reprOf` places the fields
    // and the size in one accumulation, and a second walk for an offset is the second answer that
    // eventually disagrees with the first.
    //
    // A **co-packed** field has no offset of its own: what it has is a word and a bit range in it,
    // and a caller handed the word's offset would read its neighbours. Asserted rather than
    // returned, because the two callers left are the teardown walk - which only ever reaches a field
    // that owns something, and such a field is never packed - and a body that has to ask the
    // `FieldRepr` instead.
    auto placed = reprOf(layout, record, shape).fields.get(layout.base, field);
    assertTrue(!placed.isPacked());

    return placed.offset;
}

U32 elementStrideNative(Layout layout, DeclPtr<mir::Type> element) {
    // Owned, whatever the container's own contract is: what an element costs is a property of the
    // element type, and an array reached through a borrow holds the same bytes as one this frame
    // owns. The `Array` row in `manifestNative` reaches `strideOf` with the manifestation it already
    // has, so the rounding is written once and done once.
    return strideOf(manifestNative(layout, mir::owned(element)));
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

// The declaration in `manifest.h` is where the argument is.
bool isNarrowReference(Layout layout, const mir::Occurrence& occurrence) {
    if(!isReferenceValue(layout, occurrence)) return false;
    return reprOf(layout, occurrence.type).isNarrow();
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
     * **Two types whose interior position is not a field**, and each has exactly one of them.
     *
     * A **function value** is a code word and an environment word whether or not the environment is
     * somebody else's storage, so the closure shape (`mir::Shapes::closure`) names a position that
     * is already there - and an **erased iterator object** is the same pair, so it answers here too. A **raw pointer** is one address whether or not the storage it names belongs
     * to a published origin, so `%source'a` names the pointee and changes nothing physical either -
     * which is Analysis-Lens.md §12.2's whole claim, that an interior position is a statement about
     * *provenance* and not about layout.
     *
     * `schemaOf` has nothing to answer for a structural type, so both are compared against the one
     * position their kind has rather than against a declaration that does not exist. Asserted rather
     * than skipped, because the count is what tells a renumbering from a shape the type does not
     * have - `Maybe(a'T)` against `Maybe(T)`, which arrives with generic application and *would*
     * change a layout.
     */
    if(logical.kind == mir::Type::Fun || logical.kind == mir::Type::Pointer ||
       logical.kind == mir::Type::Iter) {
        assertTrue(mir::Shapes::isEmpty(occurrence.shape) ||
                   base[occurrence.shape]->slots.size() == 1);
    } else {
        /*
         * A **record** may carry more positions than its own schema, and that is the qualified type
         * argument arriving: `Maybe('v)` against `Maybe(v)`, whose payload the signature borrowed.
         * The `Record` row below is what spends it - `layoutRecord` reads each field's contract
         * through `fieldContract` - so this is now the assertion that a shape reaches a *kind* which
         * has fields for one to name. Every other kind is a renumbering or nothing.
         */
        assertTrue(mir::Shapes::isEmpty(occurrence.shape) || logical.kind == mir::Type::Record ||
                   (schema && base[occurrence.shape]->slots.size() == base[schema]->slots.size()));
    }

    /*
     * An occurrence with a **view record** - `[T]` in a binding position, a shared `String` - is
     * laid out as that record, and the one row where the contract decides the whole answer.
     *
     * `Array(T)` owned is a growable array whose fields are the library's, and a shared or unique
     * window into one is `ArrayView(T)`, whose fields are the library's too: one logical type, two
     * contracts, two records, and nothing above this file allowed to know either. That is the third
     * layer doing its job, and the view table in `resolve_mir/type.cpp` is the one place the pairing
     * is stated - this reads it and lays the record out by the same walk every record gets.
     *
     * Above the switch because it is a fact about the *contract*, exactly as the reference-value row
     * below it is. Which logical types may carry the form is `resolve_mir`'s to police; a form set
     * on a type with no view falls through to the type's own row.
     */
    if(auto view = mir::viewRecordOf(*layout.context, *layout.program, occurrence)) {
        return manifestNative(layout, mir::owned(view));
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
        return Manifestation { LowerType(LowerLane::Pointer, 0, false), layout.target.pointerSize,
                               layout.target.pointerAlign, false, false };
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
            return Manifestation { LowerType(LowerLane::Pointer, 0, false),
                                   layout.target.pointerSize, layout.target.pointerAlign, false,
                                   false };
        /*
         * A function value - two words, `{code, env}`, and address-only like a record.
         *
         * Address-only because the lower IR has no pair, and a pair is what this is: the code word
         * alone would be a function *pointer*, which is a different thing that cannot carry
         * captures. What a call does with the two is read them back out, which is exactly what it
         * does with a record's fields.
         *
         * The size is stated on `NativeTarget` and nowhere else, and stated as *two words* rather
         * than as a number of bytes: both ends of the layout - the construction in `fun.cpp`
         * and the call that reads it back - go through `funCodeOffset`/`funEnvOffset`, and all
         * three derive from the target's pointer width.
         */
        // ... and an erased iterator object, which is the same pair: the step, and the block it
        // steps. See `mir::IterType`, where the representation is argued.
        case mir::Type::Fun:
        case mir::Type::Iter:
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), layout.target.funSize(),
                                   layout.target.pointerAlign, false, true };

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

        /*
         * A vector - the lane kind and the lane count carried across the seam unchanged, and a
         * **register** value.
         *
         * That last part is a contract rather than an observation: Design-Vector §2.5 states that a
         * vector is carried in a register and binds every calling convention to it, so a backend
         * with no register that wide has to refuse the type loudly rather than quietly spill one.
         * `inMemory` here would be that refusal made silently, at every call in the program.
         *
         * `resolve_mir` has already spent the natural form, so what arrives is a concrete count and
         * the translation is a lookup - which is the whole of why the count is resolved up there.
         * A count that is *not* concrete reaches this only inside a body compiled once, where every
         * other layout is the descriptor's too, and `dynamic` is that same answer.
         *
         * The alignment is the width. A vector move is one instruction against a naturally aligned
         * address on every target that has one, and a field holding one has to be placed where that
         * instruction may reach it.
         */
        case mir::Type::Vector: {
            auto& vector = (mir::VectorType&)logical;
            auto stride = mir::laneBytes(base, layout.target.integers, vector.lane);

            if(!vector.hasFixedCount() || !vector.lanes || !stride) {
                return Manifestation { LowerType(LowerLane::Pointer, 0, false), 0, 0, false, true,
                                       true };
            }

            auto floats = base[vector.lane]->kind == mir::Type::Float;
            auto lane = floats ? (stride == 8 ? LowerLane::Float64 : LowerLane::Float32)
                      : stride == 1 ? LowerLane::Int8 : stride == 2 ? LowerLane::Int16
                      : stride == 4 ? LowerLane::Int32 : LowerLane::Int64;

            auto width = U32(stride * vector.lanes);
            auto type = vector.isMask ? maskType(lane, vector.lanes)
                                      : vectorType(lane, vector.lanes);

            return Manifestation { type, width, width, false, false };
        }

        /*
         * An atomic is its content's bytes, at its content's width, and **in memory**.
         *
         * In memory because it is a *location* rather than a value: every operation over one takes
         * its address, and a register copy of an atomic is the second location Analysis-Atomics.md
         * §3.1 spends the type refusing. That is the one place this table departs from what the
         * logical type's content would answer on its own - a `Atomic(I32)` is four bytes like an
         * `I32` and is reached the way a record is.
         *
         * The alignment is the size rather than the content's, which for every admitted content is
         * the same number: an access a machine performs indivisibly is one that does not straddle
         * two of its words, so a location the target could not align is a location it could not
         * exchange. A content this stage cannot measure is `dynamic`, exactly as everywhere else.
         */
        case mir::Type::Atomic: {
            auto content = manifestNative(layout, mir::owned(((mir::AtomicType&)logical).content));

            if(content.dynamic) {
                return Manifestation { LowerType(LowerLane::Pointer, 0, false), 0, 0, false, true,
                                       true };
            }

            return Manifestation { content.type, content.size, content.size, false, true };
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
                auto repr = reprOf(layout, occurrence.type);
                auto lane = range.bits > 32 ? LowerLane::Int64 : LowerLane::Int32;

                return Manifestation { LowerType(lane, 0, false), repr.size, repr.align,
                                       range.isSigned, false };
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
            auto repr = reprOf(layout, occurrence.type, occurrence.shape);
            return Manifestation { LowerType(LowerLane::Pointer, 0, false), repr.size, repr.align,
                                   false, true };
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
