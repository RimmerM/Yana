/*
 * Places: where a place is, and which of the three access forms names it.
 *
 * `lowerPlace` walks a place's projections into an address. What it does *not* do is read or write
 * one, because two projections have no address to hand back - a packed field is a bit range inside
 * a word, and a folded discriminant is not stored at all - so the queries beside it are how the
 * load and the store find that out before asking for an address they cannot have. The bit
 * arithmetic each of those answers implies is lower_pack.cpp.
 */

#include "lower_internal.h"

// What a teardown's place holds. A generic teardown only ever names a whole local - a partial move
// is rejected long before here - so a root that is anything else is a concrete one by construction.
TypePtr dropPlaceType(LowerContext& lower, Function& function, const Place& place) {
    auto projections = place.projections;
    if(place.root != PlaceRoot::Local || projections.isNotEmpty()) return nullptr;
    if(place.local >= function.localCount()) return nullptr;

    return function.localAt(lower.local, place.local).type;
}

// A place becomes the address of whatever it is rooted in plus the constant offset its
// projections add up to. Nothing else survives: the lower IR has no aggregates, so this is where
// field access stops being structural and becomes arithmetic.
//
// The three roots differ only in where that first address comes from - a local's alloca, a
// global's static address, or a pointer the program computed - which is exactly why raw memory
// needs no lowering of its own beyond a root the resolver was already able to name.
LowerPtr<LowerValue> lowerPlace(LowerContext& lower, LowerBlock& block, Function& function,
                                       const Place& place, Size limit) {
    LowerPtr<LowerValue> address;

    if(place.root == PlaceRoot::Global) {
        auto global_ = lower.local[place.global];
        auto target = lower.to.globals.getValue(global_->name).unwrap();
        auto load = block.addInst(lower.lower, new (lower.to.arena) LowerInstGlobal(global_->name, target));

        address = load->created().ptr - lower.lower;
    } else if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        // A borrow is an address once the checking is done, so the two roots lower alike; all
        // that differed between them was how much could be proved before reaching here.
        address = mappedValue(lower, place.pointer);
    } else {
        assertTrue(place.local < function.localCount());

        address = mappedValue(lower, function.localAt(lower.local, place.local).value);
    }

    U32 offset = 0;

    /*
     * The path, over the walk everything shares - see resolve/place.h. What is this walk's own is
     * the offset and the address; the type each step arrives at is not, and used to be carried here
     * as well.
     *
     * `limit` stops before the trailing Property projection, which is how the *owner's* address is
     * asked for: a constrained field is reached by calling its witness with that address rather
     * than by adding anything to it. See propertySlotOf.
     */
    walkPlace(*lower.from.core, function, place, [&](const PlaceStep& step) {
        switch(step.kind) {
            case ProjectionKind::Discriminant:
                break;

            case ProjectionKind::Downcast:
                // A boxed payload sits at that offset as a pointer, and the Deref after this is
                // what loads through it - which is `step.type` already being the `%T`.
                offset += lower.repr.of(step.owner).payloadOffset;
                break;

            case ProjectionKind::Field:
                if(lower.global[step.owner]->kind == Type::Fun) {
                    offset += FunValueLayout::offsetOf(step.index);
                    break;
                }

                {
                    auto field = lower.repr.fieldOf(step.owner, step.index);
                    assertTrue(field != nullptr);
                    offset += field->offset;
                }

                break;

            case ProjectionKind::Unit:
                // The word a packed field lives in, which is the address the path has already
                // reached: a packed field's `offset` is its word's, so the Field in front of this
                // one spent it. Nothing is added - see unitBits, which is what reads the width back
                // out at the load and the store.
                break;

            case ProjectionKind::Index: {
                /*
                 * One element of a `[T *n]` - Implementation-Containers.md §6.
                 *
                 * The only projection whose step is a *value* rather than a constant, which is why
                 * it is the only one that cannot be accumulated into `offset`: the elements are `n`
                 * values at a stride and which one this is may not be known until it runs. So the
                 * constant part of the path is spent first, exactly as the Deref below spends it,
                 * and the scaled index is added to the address that produces.
                 *
                 * A constant index is spent into `offset` like a field's, which is what makes an
                 * unrolled walk over a small array cost the same as a record's fields. This used to
                 * be left to `compiler/opt` and the comment here said so, which was wrong: the
                 * multiply is built *here*, after that pass has run, so nothing folded it and every
                 * `[T *n]` access at a literal index carried a `mul` and an `add` of two constants
                 * into the backend. The literals in `FixedArray.yana.lower.expect` are what say so.
                 */
                auto stride = lower.repr.of(step.type).stride;

                if(auto index = lower.local[step.value]; index->kind == Value::ConstInt) {
                    offset += U32(((ConstInt*)index)->value * stride);
                    break;
                }

                auto from = addOffset(lower, block, address, offset);
                auto index = mappedValue(lower, step.value);
                auto scale = immediate(lower, stride);

                auto scaled = binary<LowerInst::Mul>(lower.lower, lower.to, block, lower.lower[index],
                                                     lower.lower[scale], LowerType::Int64, 0)
                    ->created().ptr - lower.lower;

                auto stepped = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[from],
                                                      lower.lower[scaled], LowerType::Pointer, 0)
                    ->created().ptr - lower.lower;

                address = stepped;
                offset = 0;
                break;
            }

            case ProjectionKind::Deref: {
                // The pointer stored here becomes the address the rest of the path is relative to,
                // so everything accumulated so far has to be spent before it is loaded.
                auto from = addOffset(lower, block, address, offset);
                auto loaded = load(lower.lower, lower.to, block, lower.lower[from], 8, false,
                                   LowerType::Pointer, 0);

                address = loaded->created().ptr - lower.lower;
                offset = 0;
                break;
            }

            case ProjectionKind::Property:
                assertTrue("unsupported place projection reached lowering" == nullptr);
                break;
        }

        return true;
    }, limit);

    return addOffset(lower, block, address, offset);
}

// What a place is rooted in, and the type its last projection is taken of. Both are the shared
// walk's - see resolve/place.h - which is what makes "by the same rules lowerPlace walks by" a fact
// rather than a hope: lowerPlace is that walk too.
/*
 * A tag that is not stored anywhere.
 *
 * Everything above turns a place into an address, which every projection but one can be. A folded
 * discriminant cannot: there is no word holding it, and its value is a *fact about the payload's
 * bits* rather than something written next to them. So it is intercepted at the load and the store
 * instead of in the place walk, which is also where it belongs - `.discriminant` is the only
 * projection whose meaning is a computation.
 */

TypePtr placeRootedType(LowerContext& lower, Function& function, const Place& place) {
    return placeRootType(*lower.from.core, function, place);
}

TypePtr placeOwnedType(LowerContext& lower, Function& function, const Place& place) {
    return placeOwnerType(*lower.from.core, function, place);
}

// The record a place's final Discriminant projection is taken of, or null where the place does not
// end in one - which is every place in a program with no sum type in it, so the cost of asking is a
// look at the last projection.
static TypePtr taggedRecord(LowerContext& lower, Function& function, const Place& place) {
    auto projections = place.projections;
    auto count = projections.size();
    if(!count) return nullptr;

    if(projections.get(lower.local, count - 1).kind != ProjectionKind::Discriminant) return nullptr;

    auto record = placeOwnedType(lower, function, place);
    if(!record || lower.global[record]->kind != Type::Record) return nullptr;

    return record;
}

// The same, narrowed to a record whose tag is folded into a niche - the one shape that has no tag in
// memory at all.
TypePtr foldedTagRecord(LowerContext& lower, Function& function, const Place& place) {
    auto record = taggedRecord(lower, function, place);
    return record && lower.repr.of(record).isNicheFolded() ? record : nullptr;
}

// And to a record whose tag is a bit range of the word its payload shares - see scalarizeSum. The
// place still lowers to the record's own address, since a bit-tagged payload begins where the record
// does; what it does not lower to is something a load of the tag's *type* would be the right width of.
TypePtr bitTagRecord(LowerContext& lower, Function& function, const Place& place) {
    auto record = taggedRecord(lower, function, place);
    return record && lower.repr.of(record).isBitTagged() ? record : nullptr;
}

/*
 * How many bytes of memory a place that names a tag actually names, or zero for a place that names
 * something else.
 *
 * A tag is the one thing a place can name whose width is not a fact about its type. Every other load
 * takes its width from what it produces; a Discriminant projection produces an `Int` whatever record
 * it was taken of, and what is in memory is however much storage that record's Repr spends on its
 * discriminant - four bytes for a payload-carrying sum, and one for a `Bool`. So the width comes from
 * the owner, and a tag load of a type narrower than `Int` zero-extends into it.
 */
U32 discriminantWidth(LowerContext& lower, Function& function, const Place& place) {
    auto record = taggedRecord(lower, function, place);
    if(!record) return 0;

    // A tag word only. The other two shapes are intercepted before anything asks for a width, and
    // answering with the containing word's here would let a missed interception store over a payload
    // rather than fail - see decodeBitTag and decodeNicheTag.
    auto& repr = lower.repr.of(record);
    return repr.discriminant == DiscriminantKind::Word ? repr.discriminantBytes : 0;
}

/*
 * Which bits of which word a place names, where it names bits rather than a word.
 *
 * The same interception a folded tag needs and for the same reason: `lowerPlace` turns a place into
 * an address, and a packed field's storage is a bit range that no address names. What the place walk
 * *does* produce is the address of the containing word, because a packed FieldRepr's `offset` is the
 * word's and every field of a scalar aggregate sits at offset zero of it - so the two halves below
 * take that address and finish the job.
 *
 * The bit offsets *compose*, which is what whole-record scalarization needs from this: in
 * `data Two {f: Flags, g: Flags}` the field `g` is two bits at bit two of a byte, and `g.a` is one bit
 * at bit two of the same byte. So the walk accumulates rather than reading the last projection, and
 * the answer is the innermost value's width at the outermost word's address.
 *
 * `exists()` is false for every place that is not one, which is every place in a program with nothing
 * packed in it.
 */
/*
 * How wide the storage unit a place names is, for the places `compiler/opt` already took apart.
 *
 * Zero for every other place, which is every place in a build with the shared expansion off and
 * every one this file still expands for itself - a reference-rooted access, or a word too wide for
 * the seam the expansion stops at. See ProjectionKind::Unit.
 *
 * The width has to come from the projection rather than from the loaded type, because they are
 * deliberately not the same number: the access is a `U32` in a register whatever the unit is, so a
 * two-field byte is one byte of traffic and a `memoryWidth` of the type would read three bytes past
 * it.
 */
U32 unitBits(LowerContext& lower, const Place& place) {
    auto projections = place.projections;
    if(projections.isEmpty()) return 0;

    auto last = projections.get(lower.local, projections.size() - 1);
    return last.kind == ProjectionKind::Unit ? last.index : 0;
}

static TypePtr narrowRefRoot(LowerContext& lower, Function& function, const Place& place);

PackedAccess packedAccess(LowerContext& lower, Function& function, const Place& place) {
    if(!placeRootedType(lower, function, place)) return {};

    /*
     * A place rooted in a reference that carries a shift is not this: the word's address is not the
     * root's value, and where the bits start is half the caller's. Those places belong to
     * `narrowRefAccess`, and keeping the two disjoint here rather than ordering the checks at each
     * call site is what stops a callee dereferencing a reference's shift as though it were an address.
     */
    if(narrowRefRoot(lower, function, place)) return {};

    PackedAccess access;
    auto declined = false;
    auto type = placeRootedType(lower, function, place);

    // The path, over the shared walk - see resolve/place.h. What is this one's own is the bit range;
    // the placement questions it asks are all about `step.owner`, which the walk hands it.
    walkPlace(*lower.from.core, function, place, [&](const PlaceStep& step) {
        auto decline = [&]() {
            declined = true;
            return false;
        };

        if(step.broken) return decline();
        type = step.type;

        switch(step.kind) {
            case ProjectionKind::Field: {
                // A function value's words are laid out by FunValueLayout rather than by Repr, and
                // are never packed - see lowerPlace, which offsets them the same way.
                if(lower.global[step.owner]->kind == Type::Fun) {
                    if(access.exists()) return decline();
                    break;
                }

                auto field = lower.repr.fieldOf(step.owner, step.index);
                if(!field) return decline();

                // A boxed field is a whole pointer, so it is neither inside a packed word nor the
                // start of one. `packCandidate` already declines it; declining here keeps that a
                // fact this walk states rather than one it assumes.
                if(step.crossedBox && access.exists()) return decline();

                if(access.exists()) {
                    /*
                     * Already inside a bit range, so this field's placement is relative to it. An
                     * *unpacked* field of a scalar aggregate is the whole of it - a single-field
                     * record keeps its address, see scalarizeTuple - so it contributes no offset and
                     * the width is the value's own.
                     */
                    access.bitOffset += field->bitOffset;
                    access.bitWidth = field->isPacked()
                        ? field->bitWidth
                        : valueWidth(lower.global, field->type).logical;

                    if(!access.bitWidth) return decline();
                } else if(field->isPacked()) {
                    access.wordBytes = field->wordBytes;
                    access.bitOffset = field->bitOffset;
                    access.bitWidth = field->bitWidth;
                }

                break;
            }
            case ProjectionKind::Downcast:
                // A payload inside a bit range can only be a single-constructor record's, whose
                // payload begins where the record does. Anything else has a tag of its own and is
                // not a scalar - see valueWidth.
                if(access.exists() && lower.repr.of(step.owner).payloadOffset) return decline();
                break;

            case ProjectionKind::Discriminant:
                // A payload-free sum *is* its discriminant, so this names the same bits under
                // another type and moves nothing. A *bit-tagged* sum's tag is at a placement of its
                // own, and one is never inside a bit range - `scalarBits` is zero for it, so nothing
                // co-packs it - but declining is what keeps that a fact rather than an assumption.
                if(lower.repr.of(step.owner).isBitTagged()) return decline();
                break;

            case ProjectionKind::Deref:
                // The pointer stored here becomes what the rest of the path is relative to, so a
                // packed word passed on the way is not this place's. Nothing narrow is a pointer, so
                // this can only be reached from outside a bit range.
                if(access.exists()) return decline();
                break;

            default:
                // A property is answered by a call taking an address rather than by a place, and is
                // intercepted before this is asked - see propertySlotOf. An `Index` steps by a
                // value, which no bit range can be reached through.
                return decline();
        }

        return true;
    });

    if(declined) return {};

    access.type = type;
    return access;
}

// The pointee type of a place that *is* a reference of this kind, or null for every other place -
// which is every place in a program with no narrow borrow in it.
static TypePtr narrowRefRoot(LowerContext& lower, Function& function, const Place& place) {
    TypePtr referenced = nullptr;

    if(place.root == PlaceRoot::Borrow) {
        /*
         * The pointee, where the root really is a borrow.
         *
         * Checked rather than assumed, and the check is not defensive tidying: a place whose root is
         * `PlaceRoot::Borrow` but whose pointer value is *not* typed `&T` reaches here, and casting
         * one to `BorrowType` reads a `to` field out of unrelated bytes - a `TypePtr` made of
         * whatever was there, and a segfault the moment anything asks for its Repr.
         *
         * That it can happen at all was found by a string format, whose sink is borrowed and written
         * through several times; which producer builds the mismatched root was not chased further,
         * because the answer here does not depend on it. A root that is not a borrow of a narrow
         * pointee is not a narrow reference, which is the only thing this function is asking, so
         * null is the correct answer rather than a fallback - and the alternative is reading a type
         * out of bytes that are not one.
         */
        auto pointee = lower.local[place.pointer]->type;
        if(!pointee || lower.global[pointee]->kind != Type::Borrow) return nullptr;

        referenced = ((BorrowType*)lower.global[pointee])->to;
    } else if(place.root == PlaceRoot::Local && place.local < function.localCount()) {
        // The slot behind a `&` parameter, which holds the reference the caller passed rather than
        // storage of its own. Every other local *is* its storage and is not one of these.
        auto slot = function.localAt(lower.local, place.local);
        if(!slot.borrowed) return nullptr;

        referenced = slot.type;
    } else {
        return nullptr;
    }

    return referenced && isNarrowRepr(lower.repr.of(referenced)) ? referenced : nullptr;
}

/*
 * Which bits of a reference a place names, or nothing where the place is not rooted in one.
 *
 * The projections a reference to a *scalar* may carry are the ones that stay inside its bits: fields
 * of a scalar aggregate, the discriminant of a payload-free sum, and the payload of a single
 * constructor. Anything else - a `Deref`, a property, a payload with a tag word of its own - leaves
 * them, and a reference whose pointee is narrow cannot have one of those under it.
 */
NarrowRefAccess narrowRefAccess(LowerContext& lower, Function& function, const Place& place) {
    auto referenced = narrowRefRoot(lower, function, place);
    if(!referenced) return {};

    NarrowRefAccess access;
    access.referenced = referenced;
    access.type = referenced;
    access.bitWidth = lower.repr.of(referenced).scalarBits;

    auto declined = false;

    // The path, over the shared walk - see resolve/place.h. `step.owner` is what this one used to
    // carry in `access.type`, and `step.crossedBox` is the boxed-edge question it used to read back
    // out of the field and the constructor for itself.
    walkPlace(*lower.from.core, function, place, [&](const PlaceStep& step) {
        auto decline = [&]() {
            declined = true;
            return false;
        };

        if(step.broken) return decline();

        // A pointer is not a bit range, and what is on the other side of it is reached by a load
        // rather than by a shift - so a path crossing a box leaves this shape entirely.
        if(step.crossedBox) return decline();

        switch(step.kind) {
            case ProjectionKind::Field: {
                auto field = lower.repr.fieldOf(step.owner, step.index);
                if(!field) return decline();

                access.bitOffset += field->bitOffset;
                access.bitWidth = field->isPacked()
                    ? field->bitWidth
                    : valueWidth(lower.global, field->type).logical;

                if(!access.bitWidth) return decline();
                break;
            }
            case ProjectionKind::Downcast:
                if(lower.repr.of(step.owner).payloadOffset) return decline();
                break;

            case ProjectionKind::Discriminant:
                // As in packedAccess: a bit-tagged sum is never behind a narrow reference, and this
                // is where that stops being something to remember.
                if(lower.repr.of(step.owner).isBitTagged()) return decline();
                break;

            default:
                return decline();
        }

        access.type = step.type;
        return true;
    });

    if(declined) return {};

    return access.bitWidth ? access : NarrowRefAccess {};
}

// The word holding the reference, for a place narrowRefType answered for.
LowerPtr<LowerValue> narrowRefValue(LowerContext& lower, Function& function, const Place& place) {
    if(place.root == PlaceRoot::Borrow) return mappedValue(lower, place.pointer);
    return mappedValue(lower, function.localAt(lower.local, place.local).value);
}
