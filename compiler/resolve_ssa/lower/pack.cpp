/*
 * The components a record has that have **no address**, and the operations over them.
 *
 * Everything here is one idea: *bits at a shift in a word*. Four things turn out to be that, and
 * each has a decode and an encode that are inverses -
 *
 *  - a field that shares a machine word with its neighbours;
 *  - a sum's tag placed above the bits its arms spend, rather than in a word of its own;
 *  - a discriminant folded into a pattern its payload cannot produce, which is the one that is not
 *    a shift and a mask but a range test - and is here because it is the other thing a projection
 *    can turn out to name;
 *  - and the two shifts a body was *handed* rather than computed: the byte offset and bit shift in a
 *    property slot, and the shift a `&` of a narrow value carries alongside its address.
 *
 * They are in one file because the masking and shifting is one piece of arithmetic parameterized,
 * and because an encode that does not undo its decode is the bug keeping the pair adjacent makes
 * visible. The last two are the reason `VirtualPlace::shift` exists: a shift that is a constant and
 * a shift that is a value are the same place, and writing them as two would be two read-modify-writes
 * to keep saying the same thing.
 *
 * *Which* of them a projection is, is `lowerInst`'s question: `field.addr` is where the record's
 * `Repr` is in hand, and the `load` or `store` that follows is where there is a direction to go in.
 * See `VirtualPlace`, which is what the two halves are joined by.
 */

#include "internal.h"

namespace ssa {

/*
 * How far up the word the component starts, as a value.
 *
 * `bitOffset` is what this body knows and `shift` is what it was handed, and the total is their sum
 * - see `VirtualPlace::shift`, where the two sources are argued. One function because both halves of
 * the pair below need the same number and a decode that shifted by a different amount than its
 * encode is the one bug this file is arranged to make impossible.
 */
LowerPtr<LowerValue> Lowering::placeShift(LowerBlock& block, const VirtualPlace& place) {
    if(!place.shift) return immediate(block, LowerType::Int64, U64(place.bitOffset));
    if(!place.bitOffset) return place.shift;

    auto within = immediate(block, LowerType::Int64, U64(place.bitOffset));
    return binary<LowerInst::Add>(lower, to, block, lower[place.shift], lower[within],
                                  LowerType::Int64, StringId())->created().ptr - lower;
}

/*
 * The widest exact load that fits in the remaining byte range. The range is the record's, not the
 * field's logical width: an access may include the field's leading or trailing partial byte, but it
 * must never reach a byte outside the record. Keeping this choice here makes the byte-wise read and
 * write paths agree about which bytes they own and gives both backends a chance to use their native
 * 32- and 64-bit memory operations.
 */
static U32 packedByteAccessWidth(U32 remaining) {
    if(remaining >= 8) return 8;
    if(remaining >= 4) return 4;
    if(remaining >= 2) return 2;
    return 1;
}

/*
 * A packed field whose access word could not cover it without leaving the record.
 *
 * The old fallback loaded every byte separately. That was correct but made a forty-bit field six
 * loads and five ORs in the common case. Read the exact range in the widest chunks available, then
 * place each chunk at its bit offset in the result. The final mask removes the unrelated bits in
 * the first and last chunks; no load overreads and no shift reaches 64.
 */
LowerPtr<LowerValue> Lowering::decodePackedBytes(LowerBlock& block, const VirtualPlace& place,
                                                 LowerType type) {
    auto span = place.spanBytes();
    LowerPtr<LowerValue> bits = nullptr;

    for(U32 byte = 0; byte < span;) {
        auto width = packedByteAccessWidth(span - byte);
        auto address = offsetAddress(block, place.word, byte);
        auto value = load(lower, to, block, lower[address], width, false, LowerType::Int64,
                          StringId())->created().ptr - lower;
        auto byteBits = byte * 8;

        if(byteBits < place.bitOffset) {
            auto shift = immediate(block, LowerType::Int64,
                                   U64(place.bitOffset - byteBits));
            value = binary<LowerInst::Shr>(lower, to, block, lower[value], lower[shift],
                                           LowerType::Int64, StringId())->created().ptr - lower;
        } else if(byteBits > place.bitOffset) {
            auto shift = immediate(block, LowerType::Int64,
                                   U64(byteBits - place.bitOffset));
            value = binary<LowerInst::Shl>(lower, to, block, lower[value], lower[shift],
                                           LowerType::Int64, StringId())->created().ptr - lower;
        }

        if(bits) {
            bits = binary<LowerInst::Or>(lower, to, block, lower[bits], lower[value],
                                         LowerType::Int64, StringId())->created().ptr - lower;
        } else {
            bits = value;
        }

        byte += width;
    }

    if(place.bitWidth < 64) {
        auto mask = immediate(block, LowerType::Int64, lowMask(place.bitWidth));
        bits = binary<LowerInst::And>(lower, to, block, lower[bits], lower[mask],
                                      LowerType::Int64, StringId())->created().ptr - lower;
    }

    if(place.isSigned && place.bitWidth < 64) {
        auto shift = immediate(block, LowerType::Int64, U64(64 - place.bitWidth));
        auto high = binary<LowerInst::Shl>(lower, to, block, lower[bits], lower[shift],
                                           LowerType::Int64, StringId())->created().ptr - lower;
        bits = binary<LowerInst::Sar>(lower, to, block, lower[high], lower[shift],
                                      LowerType::Int64, StringId())->created().ptr - lower;
    }

    return place.isSigned
        ? cast<true, true>(lower, to, block, lower[bits], type, StringId())->created().ptr - lower
        : cast<false, false>(lower, to, block, lower[bits], type, StringId())->created().ptr - lower;
}

/*
 * The write twin of `decodePackedBytes`. Each exact chunk gets one load and one store, with the
 * field's contribution shifted into that chunk before the surrounding bits are preserved. A
 * forty-bit value therefore becomes one four-byte read-modify-write and one byte read-modify-write,
 * rather than one pair for every byte.
 */
void Lowering::encodePackedBytes(LowerBlock& block, const VirtualPlace& place,
                                 LowerPtr<LowerValue> value) {
    auto widened = cast<false, false>(lower, to, block, lower[value], LowerType::Int64,
                                      StringId())->created().ptr - lower;
    auto span = place.spanBytes();
    auto fieldStart = U32(place.bitOffset);
    auto fieldEnd = fieldStart + place.bitWidth;

    for(U32 byte = 0; byte < span;) {
        auto width = packedByteAccessWidth(span - byte);
        auto byteStart = byte * 8;
        auto byteEnd = (byte + width) * 8;
        auto overlapStart = byteStart > fieldStart ? byteStart : fieldStart;
        auto overlapEnd = byteEnd < fieldEnd ? byteEnd : fieldEnd;
        auto overlapWidth = overlapEnd - overlapStart;
        auto valueShift = overlapStart - fieldStart;
        auto fieldMask = lowMask(overlapWidth);
        auto part = widened;

        if(valueShift) {
            auto shift = immediate(block, LowerType::Int64, U64(valueShift));
            part = binary<LowerInst::Shr>(lower, to, block, lower[part], lower[shift],
                                          LowerType::Int64, StringId())->created().ptr - lower;
        }

        if(overlapWidth < 64) {
            auto mask = immediate(block, LowerType::Int64, fieldMask);
            part = binary<LowerInst::And>(lower, to, block, lower[part], lower[mask],
                                          LowerType::Int64, StringId())->created().ptr - lower;
        }

        auto localOffset = overlapStart - byteStart;
        if(localOffset) {
            auto shift = immediate(block, LowerType::Int64, U64(localOffset));
            part = binary<LowerInst::Shl>(lower, to, block, lower[part], lower[shift],
                                          LowerType::Int64, StringId())->created().ptr - lower;
            fieldMask <<= localOffset;
        }

        auto address = offsetAddress(block, place.word, byte);
        auto old = load(lower, to, block, lower[address], width, false, LowerType::Int64,
                        StringId())->created().ptr - lower;
        auto clear = immediate(block, LowerType::Int64, ~fieldMask);
        auto cleared = binary<LowerInst::And>(lower, to, block, lower[old], lower[clear],
                                              LowerType::Int64, StringId())->created().ptr - lower;
        auto merged = binary<LowerInst::Or>(lower, to, block, lower[cleared], lower[part],
                                            LowerType::Int64, StringId())->created().ptr - lower;
        block.addInst(lower, new (to.arena) LowerInstStore(address, merged, width));

        byte += width;
    }
}

/*
 * Reading a packed field: load the word, move the field to the bottom, discard everything else.
 *
 * Two shapes rather than one. An unsigned field shifts down and masks; a signed one shifts *up*
 * until its sign bit is the word's and then shifts arithmetically back down, which sign-extends and
 * masks in the same two instructions rather than needing a third.
 *
 * The two constant-folding shortcuts below are exactly the ones a **known** shift buys, and neither
 * is available when the shift came out of a slot or a reference: a field at a known offset of zero
 * needs no shift at all, and one ending where its word does needs no mask, because the load is
 * unsigned at `wordBytes` and everything above the word is already zero.
 *
 * Computed in sixty-four bits throughout and narrowed at the end, which is what `kMaxPackBits` being
 * sixty-four is the other half of: a wider pack budget is a wider access path and not a free
 * extension of this one.
 */
LowerPtr<LowerValue> Lowering::decodePacked(LowerBlock& block, const VirtualPlace& place,
                                            LowerType type) {
    /*
     * A packed field can occupy an unusual number of bytes, such as five bytes for `@bits(40)`.
     * Lower loads have the machine's 1/2/4/8-byte widths only, so do not round the access up: the
     * extra bytes might belong to the next field or might not exist at all. Assemble the exact
     * span from the widest legal chunks. This path is uncommon; the normal path below remains one
     * load and the same shift/mask sequence used by every other packed component.
     */
    if(place.isBytewise()) return decodePackedBytes(block, place, type);

    auto word = load(lower, to, block, lower[place.word], place.wordBytes, false, LowerType::Int64,
                     StringId())->created().ptr - lower;

    LowerPtr<LowerValue> bits = nullptr;

    if(place.isSigned) {
        /*
         * `(word << (64 - width - shift)) >> width`, arithmetically - the sign bit of the field
         * becomes the word's and comes back down with the value. The first amount is what has to be
         * computed where the shift is not known, which is one subtract rather than a second shape.
         */
        auto top = immediate(block, LowerType::Int64, U64(64 - place.bitWidth));

        auto up = place.shift
            ? binary<LowerInst::Sub>(lower, to, block, lower[top],
                                     lower[placeShift(block, place)], LowerType::Int64,
                                     StringId())->created().ptr - lower
            : immediate(block, LowerType::Int64, U64(64 - place.bitOffset - place.bitWidth));

        auto high = binary<LowerInst::Shl>(lower, to, block, lower[word], lower[up],
                                           LowerType::Int64, StringId())->created().ptr - lower;

        bits = binary<LowerInst::Sar>(lower, to, block, lower[high], lower[top], LowerType::Int64,
                                      StringId())->created().ptr - lower;
    } else {
        bits = word;

        if(place.shift || place.bitOffset) {
            bits = binary<LowerInst::Shr>(lower, to, block, lower[bits],
                                          lower[placeShift(block, place)], LowerType::Int64,
                                          StringId())->created().ptr - lower;
        }

        if(place.shift || place.bitOffset + place.bitWidth < place.wordBytes * 8) {
            auto mask = immediate(block, LowerType::Int64, lowMask(place.bitWidth));
            bits = binary<LowerInst::And>(lower, to, block, lower[bits], lower[mask],
                                          LowerType::Int64, StringId())->created().ptr - lower;
        }
    }

    if(place.isSigned) {
        return cast<true, true>(lower, to, block, lower[bits], type, StringId())->created().ptr
             - lower;
    }

    return cast<false, false>(lower, to, block, lower[bits], type, StringId())->created().ptr
         - lower;
}

/*
 * Writing a packed field: read the word, replace the field's bits, write it back.
 *
 * The load is deliberately here rather than anywhere earlier. Design.md's write-back rule is that
 * the word is read *at commit time*, which is the whole reason two co-packed fields written across
 * one call do not lose an update - the second write reads what the first one stored. Hoisting this
 * load out of the read-modify-write, or caching a word across a call, is the classic C bit-field
 * hazard the rule exists to make impossible.
 *
 * The incoming value is masked rather than checked. That is the same choice `@bits` makes at every
 * other store, for the same reason: the mask is what makes the surrounding niche true, so it is not
 * an optimization a range check could replace.
 */
void Lowering::encodePacked(LowerBlock& block, const VirtualPlace& place,
                            LowerPtr<LowerValue> value) {
    if(place.isBytewise()) {
        encodePackedBytes(block, place, value);
        return;
    }

    auto word = load(lower, to, block, lower[place.word], place.wordBytes, false, LowerType::Int64,
                     StringId())->created().ptr - lower;

    auto widened = cast<false, false>(lower, to, block, lower[value], LowerType::Int64, StringId())
                 ->created().ptr - lower;

    auto fieldMask = immediate(block, LowerType::Int64, lowMask(place.bitWidth));
    auto trimmed = binary<LowerInst::And>(lower, to, block, lower[widened], lower[fieldMask],
                                          LowerType::Int64, StringId())->created().ptr - lower;

    auto placed = trimmed;
    LowerPtr<LowerValue> clear = nullptr;

    if(place.shift) {
        // The hole the field occupies, built from the same mask rather than from a second constant,
        // which is what keeps "which bits belong to whom" one answer when the shift is not one.
        auto amount = placeShift(block, place);

        placed = binary<LowerInst::Shl>(lower, to, block, lower[trimmed], lower[amount],
                                        LowerType::Int64, StringId())->created().ptr - lower;

        auto hole = binary<LowerInst::Shl>(lower, to, block, lower[fieldMask], lower[amount],
                                           LowerType::Int64, StringId())->created().ptr - lower;

        clear = ((LowerInstSingle*)block.addInst(lower, new (to.arena) LowerInstUnary(
            LowerInst::Not, StringId(), LowerType::Int64, hole)))->created().ptr - lower;
    } else {
        if(place.bitOffset) {
            auto shift = immediate(block, LowerType::Int64, U64(place.bitOffset));
            placed = binary<LowerInst::Shl>(lower, to, block, lower[trimmed], lower[shift],
                                            LowerType::Int64, StringId())->created().ptr - lower;
        }

        clear = immediate(block, LowerType::Int64, ~(lowMask(place.bitWidth) << place.bitOffset));
    }

    auto cleared = binary<LowerInst::And>(lower, to, block, lower[word], lower[clear],
                                          LowerType::Int64, StringId())->created().ptr - lower;

    auto merged = binary<LowerInst::Or>(lower, to, block, lower[cleared], lower[placed],
                                        LowerType::Int64, StringId())->created().ptr - lower;

    block.addInst(lower, new (to.arena) LowerInstStore(place.word, merged, place.wordBytes));
}

/*
 * Where a **bit tag** sits, as a place.
 *
 * One construction because three sites read one - the projection a match goes through, the
 * construction that writes it, and the teardown that branches on it - and a tag written at one bit
 * range and read at another is a sum that reports the wrong constructor rather than failing to
 * build. The offset is a record's own rather than a field's, which is the only thing that
 * distinguishes this from any other packed component.
 */
Lowering::VirtualPlace Lowering::discriminantPlace(LowerBlock& block, LowerPtr<LowerValue> address,
                                                   const Repr& repr) {
    VirtualPlace place;
    place.kind = VirtualPlace::Packed;
    place.word = offsetAddress(block, address, repr.discriminantOffset);
    place.wordBytes = repr.discriminantBytes;
    place.bitOffset = repr.discriminantBitOffset;
    place.bitWidth = repr.discriminantBits;

    return place;
}

/*
 * Reading a folded tag: which constructor these bits are.
 *
 * The payload constructor is "the niche word holds something the payload could legally have
 * produced", and every other constructor is one specific pattern outside that range. So the test is
 * a range check, and the answer is a select rather than a branch - which is what keeps a folded
 * `Maybe` cheaper than the tag word it replaced rather than merely smaller.
 *
 * Computed in sixty-four bits because a niche pattern can be any bit pattern of an eight-byte word,
 * and narrowed to the tag's own type at the end.
 */
LowerPtr<LowerValue> Lowering::decodeFolded(LowerBlock& block, const VirtualPlace& place,
                                            LowerType type) {
    auto repr = reprOf(layout, place.record, place.shape);
    auto& encoding = repr.encoding;
    auto& niche = encoding.niche;

    auto& record = *(mir::RecordType*)decl[place.record];
    auto constructors = record.cons.size();
    auto payload = U64(encoding.payloadConstructor);

    auto address = offsetAddress(block, place.word, niche.offset);
    auto word = load(lower, to, block, lower[address], niche.bytes, false, LowerType::Int64,
                     StringId())->created().ptr - lower;

    /*
     * `word - validStart <= validEnd - validStart`, unsigned - one subtract and one compare for a
     * range test whichever end the valid patterns sit at. The subtract disappears for the usual
     * niche, whose valid range starts at zero.
     */
    auto relative = word;
    if(niche.validStart) {
        auto start = immediate(block, LowerType::Int64, niche.validStart);
        relative = binary<LowerInst::Sub>(lower, to, block, lower[word], lower[start],
                                          LowerType::Int64, StringId())->created().ptr - lower;
    }

    auto span = immediate(block, LowerType::Int64, niche.validEnd - niche.validStart);
    auto inRange = cmp(lower, to, block, lower[relative], lower[span], LowerCmp::le, StringId())
                 ->created().ptr - lower;

    auto pick = [&](LowerPtr<LowerValue> whenInRange, LowerPtr<LowerValue> otherwise) {
        auto select = new (to.arena) LowerInstSelect(StringId(), whenInRange, otherwise, inRange,
                                                     type);
        block.addInst(lower, select);
        return select->created().ptr - lower;
    };

    /*
     * What the tag is *worth*, which is the constructor's stored number rather than its position.
     *
     * The two are the same for an ordinary sum and are not for a `@value(n)`-pinned one, and the
     * whole of what pinning costs a folded record is here rather than in the layout: the patterns
     * still stand for positions - `patternOf` hands them out in declaration order - so what changes
     * is only the last step, which turns the position this recovers back into the number.
     */
    auto valueOf = [&](U16 constructor) {
        return U64(record.cons.get(decl, constructor).value);
    };

    auto payloadValue = immediate(block, type, valueOf(encoding.payloadConstructor));

    // Two constructors is the shape this exists for - `Nothing`/`Just` and every `Outcome`-like
    // type - and there the pattern carries no information beyond "not the payload one". No
    // arithmetic, then: the answer is one of two constants, whatever they were pinned to.
    if(constructors == 2) {
        return pick(payloadValue, immediate(block, type, valueOf(U16(payload == 0 ? 1 : 0))));
    }

    /*
     * More than two, so which pattern it is decides which constructor it is. The patterns were
     * handed out to the non-payload constructors in index order, so subtracting the first recovers
     * that position. `NicheEncoding::patternOf` is the inverse of exactly this.
     */
    auto first = immediate(block, LowerType::Int64, encoding.firstPattern);
    LowerPtr<LowerValue> ordinal = nullptr;

    if(encoding.ascending) {
        ordinal = binary<LowerInst::Sub>(lower, to, block, lower[word], lower[first],
                                         LowerType::Int64, StringId())->created().ptr - lower;
    } else {
        ordinal = binary<LowerInst::Sub>(lower, to, block, lower[first], lower[word],
                                         LowerType::Int64, StringId())->created().ptr - lower;
    }

    auto narrowed = cast<false, false>(lower, to, block, lower[ordinal], type, StringId())
                  ->created().ptr - lower;

    /*
     * A sum whose numbers are its positions, which is every sum nobody pinned: the position is the
     * answer, except that the payload constructor is missing from the sequence the patterns were
     * handed out in. `position >= payloadConstructor` means this constructor was written after it,
     * so its number is one higher.
     */
    auto numbered = true;
    for(U16 i = 0; i < constructors; i++) {
        if(record.cons.get(decl, i).value != I64(i)) { numbered = false; break; }
    }

    if(numbered) {
        auto boundary = immediate(block, type, payload);
        auto shifted = cmp(lower, to, block, lower[narrowed], lower[boundary], LowerCmp::ge,
                           StringId())->created().ptr - lower;

        auto one = immediate(block, type, 1);
        auto bumped = binary<LowerInst::Add>(lower, to, block, lower[narrowed], lower[one], type,
                                             StringId())->created().ptr - lower;

        auto adjust = new (to.arena) LowerInstSelect(StringId(), bumped, narrowed, shifted, type);
        block.addInst(lower, adjust);

        return pick(payloadValue, adjust->created().ptr - lower);
    }

    /*
     * Pinned numbers, so the map from position to number is the declaration and not arithmetic -
     * one select per constructor past the first, built from the back so that the last one is the
     * fallthrough rather than a test nothing can fail.
     *
     * Branch-free, which is the property a folded tag is worth having at all: the whole point of
     * reading one is that it costs less than the word it replaced, and a jump table over three
     * constants would not. The chain is as long as the sum has constructors, and a sum that folds
     * has as many as its payload had free patterns - which for the shapes this reaches is a handful.
     */
    LowerPtr<LowerValue> answer = nullptr;

    // As many patterns as there are constructors other than the one that owns the payload, which is
    // the count `patternOf` hands them out over.
    for(I32 at = I32(constructors) - 2; at >= 0; at--) {
        // Which constructor took the `at`th pattern - the inverse of `patternOf`'s skip.
        auto constructor = U16(U32(at) >= payload ? U32(at) + 1 : U32(at));
        auto value = immediate(block, type, valueOf(constructor));

        if(!answer) { answer = value; continue; }

        auto index = immediate(block, type, U64(at));
        auto is = cmp(lower, to, block, lower[narrowed], lower[index], LowerCmp::eq, StringId())
                ->created().ptr - lower;

        auto select = new (to.arena) LowerInstSelect(StringId(), value, answer, is, type);
        block.addInst(lower, select);
        answer = select->created().ptr - lower;
    }

    return pick(payloadValue, answer);
}

/*
 * Writing a folded tag: one store, or none.
 *
 * The constructor that owns the payload is recognized by the payload being there, so building one
 * writes nothing at all - the fields the aggregate installs *are* the tag. Every other constructor
 * carries nothing, so its pattern is the whole value.
 */
void Lowering::encodeFolded(LowerBlock& block, LowerPtr<LowerValue> payload,
                            DeclPtr<mir::Type> record, DeclPtr<mir::OwnershipSchema> shape,
                            U16 constructor) {
    auto encoding = reprOf(layout, record, shape).encoding;
    if(constructor == encoding.payloadConstructor) return;

    auto address = offsetAddress(block, payload, encoding.niche.offset);
    auto pattern = immediate(block, LowerType::Int64, encoding.patternOf(constructor));

    block.addInst(lower, new (to.arena) LowerInstStore(address, pattern, encoding.niche.bytes));
}

/*
 * A packed component whose *type* has no register form - a record this target scalarized, sharing a
 * word with its neighbours.
 *
 * The bits are decoded once and put in storage of the field type's own layout, where they sit at
 * shift zero and everything below this line can read them the way it reads any other record. What
 * makes that sound rather than a copy that loses writes is where a write goes: an assignment reaches
 * a field through the projection, not through the value a load produced, so this copy is never the
 * thing anything assigns to.
 */
LowerPtr<LowerValue> Lowering::decodeIntoStorage(LowerBlock& block, const VirtualPlace& place,
                                                 const mir::Occurrence& held) {
    auto storage = frameStorage(block, held, true);
    auto manifest = manifestNative(layout, held);
    auto bits = decodePacked(block, place, packLane(manifest.size));

    block.addInst(lower, new (to.arena) LowerInstStore(storage, bits, manifest.size));
    return storage;
}

// The declaration in `internal.h` is where the argument is.
LowerPtr<LowerValue> Lowering::packedBits(LowerBlock& block, Value& value) {
    auto source = mapped(&value - ssa);
    if(!source) return nullptr;

    auto manifest = manifestNative(layout, value.occurrence);
    if(!manifest.inMemory) return source - lower;

    return load(lower, to, block, lower[source - lower], manifest.size, false,
                packLane(manifest.size), StringId())->created().ptr - lower;
}

/*
 * A **narrow reference**: an address with the shift folded into the bits above it - Design.md's
 * tier 2, and the whole of what a `&` of a co-packed field is.
 *
 * `addressBits` of the word are the address and the rest is the shift. That is sound because every
 * 64-bit target this compiler emits for leaves the top sixteen bits of a user address unused, and it
 * is a *field* of `NativeTarget` rather than a constant so that a target without the room says so.
 *
 * The whole point is that the shift folds away where it is zero: an unpacked field, a whole local,
 * anything a target left alone. `&Bool` is then exactly the address it always was, and the callee's
 * unpacking of a zero shift is what makes one compiled body serve both.
 */
LowerPtr<LowerValue> Lowering::packNarrowRef(LowerBlock& block, LowerPtr<LowerValue> address,
                                             U32 shift) {
    if(!shift) return address;
    return taggedAddress(block, address,
                         immediate(block, LowerType::Int64,
                                   U64(shift) << layout.target.addressBits));
}

/*
 * The same, where the shift is a value - a reborrow, whose incoming shift is not a constant.
 *
 * Beside the one above rather than written out at its caller, so that "the shift lives in the bits
 * from `addressBits` up" is one statement made twice at two tiers rather than in two files. The
 * constant half folds its shift in C++ and this one emits it, and that is the whole difference.
 */
LowerPtr<LowerValue> Lowering::packNarrowRef(LowerBlock& block, LowerPtr<LowerValue> address,
                                             LowerPtr<LowerValue> shift) {
    auto amount = immediate(block, LowerType::Int64, layout.target.addressBits);
    auto tag = binary<LowerInst::Shl>(lower, to, block, lower[shift], lower[amount],
                                      LowerType::Int64, StringId())->created().ptr - lower;

    return taggedAddress(block, address, tag);
}

// An address and the tag above it, joined - the arithmetic both halves end in, and the one place a
// pointer is read as a word and written back as a pointer.
LowerPtr<LowerValue> Lowering::taggedAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                             LowerPtr<LowerValue> tag) {
    auto word = reinterpret(block, address, LowerType::Int64);
    auto tagged = binary<LowerInst::Or>(lower, to, block, lower[word], lower[tag], LowerType::Int64,
                                        StringId())->created().ptr - lower;

    return reinterpret(block, tagged, LowerType::Pointer);
}

/*
 * ...and taken back apart, into the place every access below this line already knows how to spend.
 *
 * The unit is the **pointee's own** - `storageBitsFor(scalarBits)`, which is a constant the callee
 * reads out of its own layout table for a type its signature named. That is the half of the ABI the
 * reference does not carry, and the reason it does not have to: what a callee is missing about a
 * narrow field is where it starts, not how wide it is.
 *
 * `bitOffset` stays zero and the shift goes in `VirtualPlace::shift`, so that a field projected
 * *through* this reference adds its own constant to the constant half - see `VirtualPlace::shift`,
 * where the two are argued.
 */
Lowering::VirtualPlace Lowering::unpackNarrowRef(LowerBlock& block, LowerPtr<LowerValue> reference,
                                                 DeclPtr<mir::Type> pointee) {
    auto repr = reprOf(layout, pointee);
    auto addressBits = layout.target.addressBits;

    /*
     * The bit arithmetic runs on an integer and the result becomes an address again.
     *
     * Only `add` and `sub` take a pointer operand in the lower IR, which is the right rule - so this
     * says what it is doing rather than relying on a pointer and a word being the same width.
     */
    auto word = reinterpret(block, reference, LowerType::Int64);

    auto mask = immediate(block, LowerType::Int64, lowMask(addressBits));
    auto masked = binary<LowerInst::And>(lower, to, block, lower[word], lower[mask],
                                         LowerType::Int64, StringId())->created().ptr - lower;

    auto amount = immediate(block, LowerType::Int64, U64(addressBits));

    VirtualPlace place;
    place.kind = VirtualPlace::Packed;
    place.word = reinterpret(block, masked, LowerType::Pointer);
    place.wordBytes = U32(mir::storageBitsFor(U8(repr.scalarBits)) / 8);
    place.bitWidth = repr.scalarBits;
    place.isSigned = manifestNative(layout, mir::owned(pointee)).isSigned;

    place.shift = binary<LowerInst::Shr>(lower, to, block, lower[word], lower[amount],
                                         LowerType::Int64, StringId())->created().ptr - lower;

    return place;
}

/*
 * Building one, at the borrow.
 *
 * The shift is split against the unit the *callee* will load in, which is the pointee's rather than
 * the word the field is actually in: a field at bit 20 of a four-byte word, whose type is a byte
 * wide, is bit 4 of the third byte. Every operand is a constant where the incoming place is one, so
 * borrowing an unpacked field costs nothing at all.
 *
 * ## Why the split is sound, and the one case it is not
 *
 * The callee loads `storageBitsFor(bits)` at the address, so the field has to lie inside one such
 * block. `packBits` guarantees exactly that for a field against the *word* it was placed in - it is
 * the straddle rule, and it is in the layout for this reason and no other - and it composes through
 * a nested packed record because a record whose span exceeds a byte is only ever placed at a
 * multiple of its own storage unit: the fields are placed widest first, so anything before it is at
 * least as wide, and a record of `n > 8` bits has `n > storageBitsFor(n)/2`, which leaves no room
 * for a narrower run in front of it inside one unit. A record of eight bits or fewer lies inside one
 * byte, and every unit inside it is that byte.
 *
 * The exception is a `C` pin, where the unit is the *declared* type's and a four-bit field may
 * legally sit at bit six of an `int` - crossing the byte a `&@bits(4) U8` would load. C has the same
 * hole and answers it by refusing to take the address of a bit-field at all, which is what this does.
 */
LowerPtr<LowerValue> Lowering::borrowNarrow(LowerBlock& block, Value& result,
                                            const VirtualPlace& place, LocationId source) {
    if(place.isBytewise()) {
        context.diagnostics.error(
            "cannot borrow this field - its packed representation has no addressable storage unit"_v,
            source);
        return place.word;
    }

    auto pointee = result.occurrence.type;
    auto unitBits = U32(mir::storageBitsFor(U8(reprOf(layout, pointee).scalarBits)));

    if(!place.shift) {
        if(place.bitOffset % unitBits + place.bitWidth > unitBits) {
            context.diagnostics.error(
                "cannot borrow this field - its bits cross the storage unit a reference to its type would load, which a `@layout(c)` bit-field is allowed to do and a reference cannot describe"_v,
                source);

            return place.word;
        }

        auto step = place.bitOffset / unitBits * (unitBits / 8);
        return packNarrowRef(block, offsetAddress(block, place.word, step),
                             place.bitOffset % unitBits);
    }

    /*
     * A reborrow: the incoming shift is not a constant, so the split is arithmetic. Both halves are
     * exact because the unit is a power of two - `(total / unitBits) * unitBytes` is two shifts, and
     * what is left over is one mask.
     */
    U32 unitLog = 0;
    while((U32(1) << unitLog) < unitBits) unitLog++;

    auto total = placeShift(block, place);

    auto units = binary<LowerInst::Shr>(lower, to, block, lower[total],
                                        lower[immediate(block, LowerType::Int64, unitLog)],
                                        LowerType::Int64, StringId())->created().ptr - lower;

    auto step = binary<LowerInst::Shl>(lower, to, block, lower[units],
                                       lower[immediate(block, LowerType::Int64, unitLog - 3)],
                                       LowerType::Int64, StringId())->created().ptr - lower;

    auto address = binary<LowerInst::Add>(lower, to, block, lower[place.word], lower[step],
                                          LowerType::Pointer, StringId())->created().ptr - lower;

    auto within = binary<LowerInst::And>(lower, to, block, lower[total],
                                         lower[immediate(block, LowerType::Int64, unitBits - 1)],
                                         LowerType::Int64, StringId())->created().ptr - lower;

    return packNarrowRef(block, address, within);
}

/*
 * A narrow reference **materialized** - Design.md's tier 1, at the one boundary tier 2 cannot cross.
 *
 * A body compiled once reaches a value of a type parameter through a plain address and takes its
 * size out of a descriptor, so a reference carrying a shift is one it would dereference at an
 * address sixteen bits too large. There is nowhere to put the shift, because the whole point of that
 * body is that it was compiled without knowing the type.
 *
 * So at that boundary the value is copied into storage of its own, where it sits at shift zero, and
 * copied back when the call returns. "When the call returns" is what makes it sound, and it is why
 * the commit is keyed to the call rather than recorded against the borrow: a write through a
 * reference becomes visible when the callee is done with it, and the word is re-read at that moment
 * - which is what stops two co-packed fields passed to one call from losing an update.
 */
Lowering::PackedBorrow Lowering::materializeNarrow(LowerBlock& block, LowerPtr<LowerValue> reference,
                                                   const mir::Occurrence& held) {
    auto place = unpackNarrowRef(block, reference, held.type);
    auto manifest = manifestNative(layout, mir::owned(held.type));

    auto storage = frameStorage(block, mir::owned(held.type), true);
    auto bits = decodePacked(block, place, packLane(manifest.size));

    block.addInst(lower, new (to.arena) LowerInstStore(storage, bits, manifest.size));
    return PackedBorrow { place, storage, manifest.size };
}

// The other half: what the callee wrote into the temporary, put back into the word it came from.
void Lowering::commitPackedBorrow(LowerBlock& block, const PackedBorrow& borrow) {
    auto lane = packLane(borrow.bytes);
    auto value = load(lower, to, block, lower[borrow.temporary], borrow.bytes, false, lane,
                      StringId())->created().ptr - lower;

    encodePacked(block, borrow.place, value);
}

// One word read as another - the lower IR keeps pointers and integers apart, which is the right rule
// and the reason a reference that carries a number in it has to say when it is being one.
LowerPtr<LowerValue> Lowering::reinterpret(LowerBlock& block, LowerPtr<LowerValue> value,
                                           LowerType type) {
    return ((LowerInstSingle*)block.addInst(lower, new (to.arena) LowerInstUnary(
        LowerInst::Bitcast, StringId(), type, value)))->created().ptr - lower;
}

/*
 * What a narrow reference operand's pointee is, looked **through the `bitcast` that restates one as
 * a raw pointer** - `addressOf` of a `&` argument, which is what `swap` and `exchange` expand to.
 *
 * The bitcast is value-preserving at this tier: a reference and a `%T` are both `LowerType::Pointer`
 * and the instruction copies the bits, shift and all. What it does change is the *occurrence*, and
 * `isNarrowReference` is a question about an occurrence - so the raw pointer answered no and the
 * load and the store below it dereferenced a tagged address. That is a segfault rather than a wrong
 * answer, and it was `ScalarRecord.yana`'s.
 *
 * One step and not a walk. `expandExchange` and `expandPointerLoad` each emit exactly one, and a
 * chain of bitcasts between two pointer types is not a shape any expansion builds; a loop here would
 * be a guess about a producer that does not exist.
 */
DeclPtr<mir::Type> Lowering::narrowPointee(Value& value) {
    if(isNarrowReference(layout, value.occurrence)) return value.occurrence.type;
    if(!value.definer) return nullptr;

    auto& definer = *ssa[value.definer];
    if(definer.kind != Inst::Unary) return nullptr;

    auto& written = (InstUnary&)definer;
    if(written.op != mir::UnaryOp::Bitcast) return nullptr;

    auto& operand = *ssa[written.value.value()];
    if(!isNarrowReference(layout, operand.occurrence)) return nullptr;

    return operand.occurrence.type;
}

/*
 * The component an address operand names, wherever the operand does not name storage.
 *
 * Two sources and one answer, which is the point: a projection this body made against a record it
 * can see left a `VirtualPlace` behind, and a **reference** to something narrow carries one in its
 * own bits. Everything that reads or writes through a place asks this rather than the table, so that
 * a body holding a `&Flags` reaches `f.a` through the same three lines a body holding a `Flags` does.
 */
Maybe<Lowering::VirtualPlace> Lowering::narrowPlace(LowerBlock& block, Value& value,
                                                    LowerPtr<LowerValue> address) {
    if(auto found = virtualPlaces.getValue(value.id.index)) return found;

    auto pointee = narrowPointee(value);
    if(!pointee) return Maybe<VirtualPlace>();

    return Maybe<VirtualPlace>(unpackNarrowRef(block, address, pointee));
}

/*
 * Reading one - the four shapes, and which applies is the *result's* question rather than the
 * place's.
 *
 * A folded tag decodes to a constructor number; a `&` of the component is a reference that carries
 * the shift; a value whose type has no register form goes into storage of its own; and everything
 * else is the shift and the mask in a register. One function because a load, a load through a
 * reference and a copy all reach the same four, and three copies of the ladder is how one of them
 * comes to miss a row.
 */
void Lowering::spendPlace(LowerBlock& block, Value& result, const VirtualPlace& place,
                          LocationId source) {
    auto manifest = manifestNative(layout, result.occurrence);

    if(place.kind == VirtualPlace::Folded) {
        values.add(result.id.index, decodeFolded(block, place, manifest.type));
    } else if(isReferenceValue(layout, result.occurrence)) {
        values.add(result.id.index, borrowNarrow(block, result, place, source));
    } else if(manifest.inMemory) {
        values.add(result.id.index, decodeIntoStorage(block, place, result.occurrence));
    } else {
        values.add(result.id.index, decodePacked(block, place, manifest.type));
    }
}

/*
 * Whether every user of this projection is one that can be served without an address.
 *
 * A packed field has none - that is the honest end of the trade-off Design.md names, and a field
 * that gave up its address to share a word has given it up to everything that wanted one. What is
 * allowed is the four that go through this file: a `load`, a `store`, a `copy`, and a projection one
 * level deeper, which composes its shift onto this one rather than dereferencing anything.
 *
 * A `&` is one of them now. Tier 2 answers it - `borrowNarrow`, which puts the shift in the bits
 * above the address - so what is left here is the operations that want a *machine* address and can
 * carry nothing beside it: a `%T` taken of the field, and anything reached through one. The shipping
 * compiler reports the same thing about `%T` at the same place in the source, for the same reason.
 */
bool Lowering::checkAddressable(Value& result, DeclPtr<mir::Type> record, U16 field,
                                LocationId source) {
    auto& declared = *(mir::RecordType*)decl[record];
    auto placed = reprOf(layout, record).fields.get(decl, field);

    for(auto used: result.uses(ssa)) {
        switch(ssa[used]->kind) {
            case Inst::Load:
            case Inst::Store:
            case Inst::Copy:
            case Inst::FieldAddress:
                continue;
            default:
                break;
        }

        if(placed.isBytewise()) {
            context.diagnostics.error(
                "cannot borrow %@ - its packed field spans a byte range that cannot be represented by the field type's address"_v,
                source, context.findName(declared.fields.get(decl, field).name));
        } else {
            context.diagnostics.error(
                "cannot take the address of %@ - it shares a machine word with its neighbours, so it has no address of its own"_v,
                source, context.findName(declared.fields.get(decl, field).name));
        }

        return false;
    }

    return true;
}

} // namespace ssa
