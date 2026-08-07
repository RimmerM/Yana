/*
 * Encoding: reading a value out of a bit range, and writing one back into it.
 *
 * Four representations share one shape - a packed field, a folded (niche) tag, a bit tag, and a
 * narrow reference - and each has a decode and an encode that are inverses. They are together
 * because the masking, shifting and sign extension is one piece of arithmetic parameterized four
 * ways, and because an encode that does not undo its decode is the bug this file exists to make
 * visible by keeping the pair adjacent.
 *
 * Which of the four a place is, is lower_place.cpp's question.
 */

#include "lower_internal.h"

/*
 * Reading a folded tag: which constructor these bits are.
 *
 * The payload constructor is "the niche word holds something the payload could legally have
 * produced", and every other constructor is one specific pattern outside that range. So the test is
 * a range check, and the answer is a select rather than a branch - which is what keeps a folded
 * `Maybe` cheaper than the tag word it replaced rather than merely smaller.
 *
 * Computed in 64 bits throughout because a niche pattern can be any bit pattern of an eight-byte
 * word, and narrowed to the tag's own type at the end.
 */
LowerPtr<LowerValue> decodeNicheTag(LowerContext& lower, LowerBlock& block,
                                           LowerPtr<LowerValue> payload, TypePtr record,
                                           TypePtr tagType, StringId name) {
    auto& repr = lower.repr.of(record);
    auto& encoding = repr.encoding;
    auto& niche = encoding.niche;

    auto constructors = ((RecordType*)lower.global[record])->constructors.size();
    auto payloadIndex = U64(encoding.payloadConstructor);

    auto address = addOffset(lower, block, payload, niche.offset);
    auto loaded = load(lower.lower, lower.to, block, lower.lower[address], niche.bytes, false,
                       LowerType::Int64, StringId());
    auto word = loaded->created().ptr - lower.lower;

    /*
     * `word - validStart <= validEnd - validStart`, unsigned - one subtract and one compare for a
     * range test whichever end the valid patterns sit at. The subtract disappears for the usual
     * niche, whose valid range starts at zero.
     */
    auto relative = word;
    if(niche.validStart) {
        auto base = immediate(lower, niche.validStart);
        relative = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[word],
                                          lower.lower[base], LowerType::Int64, StringId())->created().ptr - lower.lower;
    }

    auto span = immediate(lower, niche.validEnd - niche.validStart);
    auto inRange = cmp(lower.lower, lower.to, block, lower.lower[relative], lower.lower[span],
                       LowerCmp::le, StringId())->created().ptr - lower.lower;

    auto tagLower = lowerType(lower.global, tagType);
    auto pick = [&](LowerPtr<LowerValue> whenInRange, LowerPtr<LowerValue> otherwise) {
        auto select = new (lower.to.arena) LowerInstSelect(name, whenInRange, otherwise, inRange, tagLower);
        block.addInst(lower.lower, select);
        return select->created().ptr - lower.lower;
    };

    // Two constructors is the shape this exists for - `Nothing`/`Just` and every `Result`-like type -
    // and there the pattern carries no information beyond "not the payload one". No arithmetic, then:
    // the answer is one of two constants.
    if(constructors == 2) {
        auto payloadTag = immediate(lower, payloadIndex, tagLower);
        auto otherTag = immediate(lower, payloadIndex == 0 ? 1 : 0, tagLower);
        return pick(payloadTag, otherTag);
    }

    /*
     * More than two, so which pattern it is decides which constructor it is. The patterns were handed
     * out to the non-payload constructors in index order, so recovering the ordinal recovers the
     * index - except that the payload constructor is missing from that sequence, which the last step
     * puts back.
     */
    auto first = immediate(lower, encoding.firstPattern);
    LowerPtr<LowerValue> ordinal;

    if(encoding.ascending) {
        ordinal = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[word],
                                         lower.lower[first], LowerType::Int64, StringId())->created().ptr - lower.lower;
    } else {
        ordinal = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[first],
                                         lower.lower[word], LowerType::Int64, StringId())->created().ptr - lower.lower;
    }

    auto narrowed = cast<false, false>(lower.lower, lower.to, block, lower.lower[ordinal],
                                       tagLower, StringId())->created().ptr - lower.lower;

    // `ordinal >= payloadConstructor` means this constructor was written after the payload one, so
    // its index is one higher than its position in the pattern sequence.
    auto boundary = immediate(lower, payloadIndex, tagLower);
    auto shifted = cmp(lower.lower, lower.to, block, lower.lower[narrowed], lower.lower[boundary],
                       LowerCmp::ge, StringId())->created().ptr - lower.lower;

    auto one = immediate(lower, 1, tagLower);
    auto bumped = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[narrowed],
                                         lower.lower[one], tagLower, StringId())->created().ptr - lower.lower;

    auto adjust = new (lower.to.arena) LowerInstSelect(StringId(), bumped, narrowed, shifted, tagLower);
    block.addInst(lower.lower, adjust);

    auto payloadTag = immediate(lower, payloadIndex, tagLower);
    return pick(payloadTag, adjust->created().ptr - lower.lower);
}

/*
 * Reading a packed field: load the word, move the field to the bottom, discard everything else.
 *
 * Two shapes rather than one. An unsigned field shifts down and masks; a signed one shifts *up*
 * until its sign bit is the word's and then shifts arithmetically back down, which sign-extends and
 * masks in the same two instructions rather than needing a third.
 *
 * The mask covers the bits *above* the range, so a field ending where its word does has none: the
 * load is unsigned at `wordBytes`, so everything above the word is already zero and the shift that
 * brought the field down took the rest with it. Same condition as `decode` in opt/opt_pack.cpp, which
 * handles every access this one does not.
 */
LowerPtr<LowerValue> decodePackedBits(LowerContext& lower, LowerBlock& block,
                                             LowerPtr<LowerValue> word, const PackedAccess& field,
                                             bool isSigned) {
    auto loaded = load(lower.lower, lower.to, block, lower.lower[word], field.wordBytes, false,
                       LowerType::Int64, StringId());
    auto bits = loaded->created().ptr - lower.lower;

    if(isSigned) {
        auto up = immediate(lower, 64 - field.bitOffset - field.bitWidth);
        auto down = immediate(lower, 64 - field.bitWidth);

        auto high = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[bits],
                                           lower.lower[up], LowerType::Int64, StringId())->created().ptr - lower.lower;

        return binary<LowerInst::Sar>(lower.lower, lower.to, block, lower.lower[high],
                                      lower.lower[down], LowerType::Int64, StringId())->created().ptr - lower.lower;
    }

    auto value = bits;
    if(field.bitOffset) {
        auto shift = immediate(lower, field.bitOffset);
        value = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[value],
                                       lower.lower[shift], LowerType::Int64, StringId())->created().ptr - lower.lower;
    }

    if(field.bitOffset + field.bitWidth >= U32(field.wordBytes) * 8) return value;

    auto mask = immediate(lower, lowMask(field.bitWidth));
    return binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[value],
                                  lower.lower[mask], LowerType::Int64, StringId())->created().ptr - lower.lower;
}

LowerPtr<LowerValue> decodePackedField(LowerContext& lower, LowerBlock& block,
                                              LowerPtr<LowerValue> word, const PackedAccess& field,
                                              TypePtr type, StringId name) {
    auto bits = decodePackedBits(lower, block, word, field, signedType(lower.global, type));
    auto result = lowerType(lower.global, type);

    if(signedType(lower.global, type)) {
        return cast<true, true>(lower.lower, lower.to, block, lower.lower[bits], result, name)
            ->created().ptr - lower.lower;
    }

    return cast<false, false>(lower.lower, lower.to, block, lower.lower[bits], result, name)
        ->created().ptr - lower.lower;
}

/*
 * Writing a packed field: read the word, replace the field's bits, write it back.
 *
 * The load is deliberately here rather than anywhere earlier. Design.md's write-back rule is that
 * the word is read *at commit time*, which is the whole reason two co-packed fields borrowed across
 * one call do not lose an update - the second commit reads what the first one wrote. Hoisting this
 * load out of the read-modify-write, or caching a word across a call, reintroduces the classic C
 * bitfield hazard that the rule exists to make impossible.
 *
 * The incoming value is masked rather than checked. That is the same choice `@bits` makes at every
 * other store, for the same reason: the mask is what makes the surrounding niche true, so it is not
 * an optimization that a range check could replace.
 */
void encodePackedField(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> word,
                              const PackedAccess& field, LowerPtr<LowerValue> value) {
    auto loaded = load(lower.lower, lower.to, block, lower.lower[word], field.wordBytes, false,
                       LowerType::Int64, StringId());
    auto bits = loaded->created().ptr - lower.lower;

    auto widened = cast<false, false>(lower.lower, lower.to, block, lower.lower[value],
                                      LowerType::Int64, StringId())->created().ptr - lower.lower;

    auto fieldMask = immediate(lower, lowMask(field.bitWidth));
    auto trimmed = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[widened],
                                          lower.lower[fieldMask], LowerType::Int64, StringId())->created().ptr - lower.lower;

    auto placed = trimmed;
    if(field.bitOffset) {
        auto shift = immediate(lower, field.bitOffset);
        placed = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[trimmed],
                                        lower.lower[shift], LowerType::Int64, StringId())->created().ptr - lower.lower;
    }

    auto clearMask = immediate(lower, ~(lowMask(field.bitWidth) << field.bitOffset));
    auto cleared = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[bits],
                                          lower.lower[clearMask], LowerType::Int64, StringId())->created().ptr - lower.lower;

    auto merged = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[cleared],
                                        lower.lower[placed], LowerType::Int64, StringId())->created().ptr - lower.lower;

    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(word, merged, field.wordBytes));
}

// The address and the shift, taken back apart. Two instructions, and the mask is the only constant
// the target contributes.
NarrowRef unpackNarrowRef(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> ref,
                                 const NarrowRefAccess& access) {
    // The unit is the *pointee's*, not the accessed field's, which is what makes a field of a scalar
    // aggregate reachable through a reference to the whole of it: the aggregate's bits all sit inside
    // one such unit, so a load of it covers whichever field the constant below selects.
    auto unitBits = naturalStorageBits(lower.repr.of(access.referenced).scalarBits);
    auto addressBits = lower.repr.target.addressBits;

    // The bit arithmetic runs on an integer and the result becomes an address again. Only Add and
    // Sub take a pointer operand in the lower IR - see validateArith - which is the right rule and
    // is why this says what it is doing rather than relying on the two being the same width.
    auto word = reinterpret(lower, block, ref, LowerType::Int64);

    auto addressMask = immediate(lower, lowMask(addressBits));
    auto masked = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[word],
                                         lower.lower[addressMask], LowerType::Int64, StringId())
        ->created().ptr - lower.lower;

    auto address = reinterpret(lower, block, masked, LowerType::Pointer);

    auto shiftBy = immediate(lower, addressBits);
    auto shift = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[word],
                                        lower.lower[shiftBy], LowerType::Int64, StringId())
        ->created().ptr - lower.lower;

    // Where the field sits inside the pointee, added to where the pointee sits inside its unit. This
    // constant is the callee's own - it read it out of its Repr for a type it was told - so nothing
    // about it had to travel in the reference.
    if(access.bitOffset) {
        auto within = immediate(lower, access.bitOffset);
        shift = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[shift],
                                       lower.lower[within], LowerType::Int64, StringId())
            ->created().ptr - lower.lower;
    }

    return NarrowRef {
        .address = address,
        .shift = shift,
        .unitBytes = unitBits / 8,
        .bits = access.bitWidth,
        .isSigned = signedType(lower.global, access.type),
    };
}

// Building one, at the borrow. `shift` is a constant here - the borrow site knows exactly which
// field it is taking - and folds away entirely for the unpacked case, where it is zero.
LowerPtr<LowerValue> packNarrowRef(LowerContext& lower, LowerBlock& block,
                                          LowerPtr<LowerValue> address, U32 shift) {
    if(!shift) return address;

    auto word = reinterpret(lower, block, address, LowerType::Int64);

    auto tag = immediate(lower, U64(shift) << lower.repr.target.addressBits);
    auto tagged = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[word],
                                        lower.lower[tag], LowerType::Int64, StringId())->created().ptr - lower.lower;

    return reinterpret(lower, block, tagged, LowerType::Pointer);
}

/*
 * A reference to something *inside* what a reference already names.
 *
 * `ref.shift` is where the value starts inside the word, which is the two halves of a reference added
 * together - the shift the caller passed plus the field's own offset. What is left is to re-split that
 * total against the unit the new pointee will be loaded in, since it may be narrower than the one the
 * old shift was measured against: four bits at bit 9 of a sixteen-bit unit are bit 1 of the second
 * byte, and a callee holding a `&@bits(4)` loads a byte.
 *
 * Every operand is a constant when the incoming shift was one, so a reborrow of a field of a whole
 * local costs nothing at all.
 */
LowerPtr<LowerValue> stepNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                                          U32 unitBytes) {
    auto unitBits = unitBytes * 8;
    U32 unitLog = 0;
    while((U32(1) << unitLog) < unitBits) unitLog++;

    // (total / unitBits) * unitBytes, as two shifts, which is exact because both are powers of two.
    auto units = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[ref.shift],
                                        lower.lower[immediate(lower, unitLog)], LowerType::Int64, StringId())
        ->created().ptr - lower.lower;
    auto step = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[units],
                                       lower.lower[immediate(lower, unitLog - 3)], LowerType::Int64, StringId())
        ->created().ptr - lower.lower;

    auto address = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[ref.address],
                                          lower.lower[step], LowerType::Pointer, StringId())
        ->created().ptr - lower.lower;

    auto within = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[ref.shift],
                                         lower.lower[immediate(lower, unitBits - 1)], LowerType::Int64, StringId())
        ->created().ptr - lower.lower;

    auto word = reinterpret(lower, block, address, LowerType::Int64);
    auto tag = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[within],
                                      lower.lower[immediate(lower, lower.repr.target.addressBits)],
                                      LowerType::Int64, StringId())->created().ptr - lower.lower;
    auto tagged = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[word],
                                        lower.lower[tag], LowerType::Int64, StringId())->created().ptr - lower.lower;

    return reinterpret(lower, block, tagged, LowerType::Pointer);
}

// Reading through one: the same two shapes decodePackedField has, with the shift loaded rather than
// written in.
LowerPtr<LowerValue> decodeNarrowBits(LowerContext& lower, LowerBlock& block, const NarrowRef& ref) {
    auto loaded = load(lower.lower, lower.to, block, lower.lower[ref.address], ref.unitBytes, false,
                       LowerType::Int64, StringId());
    auto word = loaded->created().ptr - lower.lower;

    auto shifted = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[word],
                                          lower.lower[ref.shift], LowerType::Int64, StringId())
        ->created().ptr - lower.lower;

    auto mask = immediate(lower, lowMask(ref.bits));
    auto masked = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[shifted],
                                         lower.lower[mask], LowerType::Int64, StringId())->created().ptr - lower.lower;

    // Sign extension, where the type has a sign to extend: shift the value's top bit up to the
    // word's and bring it back arithmetically.
    if(ref.isSigned) {
        auto up = immediate(lower, 64 - ref.bits);
        auto high = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[masked],
                                           lower.lower[up], LowerType::Int64, StringId())->created().ptr - lower.lower;

        return binary<LowerInst::Sar>(lower.lower, lower.to, block, lower.lower[high],
                                      lower.lower[up], LowerType::Int64, StringId())->created().ptr - lower.lower;
    }

    return masked;
}

LowerPtr<LowerValue> decodeNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                                            TypePtr type, StringId name) {
    auto bits = decodeNarrowBits(lower, block, ref);
    auto result = lowerType(lower.global, type);

    if(ref.isSigned) {
        return cast<true, true>(lower.lower, lower.to, block, lower.lower[bits], result, name)
            ->created().ptr - lower.lower;
    }

    return cast<false, false>(lower.lower, lower.to, block, lower.lower[bits], result, name)
        ->created().ptr - lower.lower;
}

/*
 * Writing through one, which is a read-modify-write of the unit and has no commit point.
 *
 * That is the whole of what makes this representation able to outlive the call that produced it:
 * there is no temporary to write back, so there is nothing whose lifetime has to be arranged. Every
 * write is complete when it returns, and two references into one unit interleave safely because each
 * reads the unit as it stands.
 */
void encodeNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                            LowerPtr<LowerValue> value) {
    auto loaded = load(lower.lower, lower.to, block, lower.lower[ref.address], ref.unitBytes, false,
                       LowerType::Int64, StringId());
    auto word = loaded->created().ptr - lower.lower;

    auto widened = cast<false, false>(lower.lower, lower.to, block, lower.lower[value],
                                      LowerType::Int64, StringId())->created().ptr - lower.lower;

    auto mask = immediate(lower, lowMask(ref.bits));
    auto trimmed = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[widened],
                                          lower.lower[mask], LowerType::Int64, StringId())->created().ptr - lower.lower;
    auto placed = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[trimmed],
                                         lower.lower[ref.shift], LowerType::Int64, StringId())->created().ptr - lower.lower;

    auto hole = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[mask],
                                       lower.lower[ref.shift], LowerType::Int64, StringId())->created().ptr - lower.lower;
    auto keep = unary<LowerInst::Not>(lower.lower, lower.to, block, lower.lower[hole],
                                      LowerType::Int64, StringId())->created().ptr - lower.lower;

    auto cleared = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[word],
                                          lower.lower[keep], LowerType::Int64, StringId())->created().ptr - lower.lower;
    auto merged = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[cleared],
                                        lower.lower[placed], LowerType::Int64, StringId())->created().ptr - lower.lower;

    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(ref.address, merged, ref.unitBytes));
}

/*
 * Storage of its own, for a scalar aggregate that was living in someone else's word.
 *
 * Reading `two.g` produces a `Flags`, and a `Flags` is a memory type - every consumer of one expects
 * an address. What the bits were part of is a word this frame does not own the rest of, so the value
 * has to be somewhere before it can be handed over, and its own scalar storage is exactly as wide as
 * the bits are: `naturalBytes(scalarBits)`, with the fields at the same bit offsets they have here.
 *
 * That is the whole cost of scalarizing an aggregate rather than making it a register value. A
 * *direct* scalar record would need no storage at all, and this alloca is where that shows up - see
 * isDirectType, which is target-independent and therefore cannot know that this record became one.
 */
LowerPtr<LowerValue> materializeScalar(LowerContext& lower, LowerBlock& block, TypePtr type,
                                              LowerPtr<LowerValue> bits, StringId name) {
    auto& repr = lower.repr.of(type);
    auto bytes = immediate(lower, repr.size);
    auto storage = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(name, bytes, repr.align));
    auto address = storage->created().ptr - lower.lower;

    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, bits, repr.size));
    return address;
}

// The other direction: the bits of a value about to be written into a word. A scalar aggregate arrives
// as the address of its own storage, so its bits are a load of it; everything else already is its
// bits. The mask that trims it to the field's width is applied by whoever merges, so a scalar with
// unused high bits needs nothing done to it here.
LowerPtr<LowerValue> scalarBitsOf(LowerContext& lower, LowerBlock& block, TypePtr type,
                                         LowerPtr<LowerValue> value) {
    if(!type || !isMemoryType(lower.global, type)) return value;

    auto& repr = lower.repr.of(type);
    auto loaded = load(lower.lower, lower.to, block, lower.lower[value], repr.size, false,
                       LowerType::Int64, StringId());

    return loaded->created().ptr - lower.lower;
}

/*
 * Writing a folded tag, which for the payload constructor is writing nothing at all.
 *
 * That is not an optimization but the definition: the payload constructor *is* the payload's own
 * bits, so the only thing that could make it identifiable is the payload being written, which the
 * constructor's own field initializations do. Every other constructor has no payload to write, so
 * its pattern is the whole value.
 */
void encodeNicheTag(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> payload,
                           TypePtr record, U64 constructor) {
    auto& encoding = lower.repr.of(record).encoding;
    if(constructor == encoding.payloadConstructor) return;

    auto address = addOffset(lower, block, payload, encoding.niche.offset);
    auto pattern = immediate(lower, encoding.patternOf(U16(constructor)));
    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, pattern, encoding.niche.bytes));
}

/*
 * A tag that is a bit range of the word its payload sits in - see scalarizeSum.
 *
 * The third of the three tag shapes, and the one with the least of its own: where a folded tag is a
 * range check over the payload and a tag word is an ordinary load, this is exactly a co-packed field,
 * so both directions are the packed access path with the tag's own placement handed to it. What it
 * needs from the record's Repr is a `PackedAccess`, and that is all it needs.
 */
PackedAccess bitTagAccess(const Repr& repr) {
    PackedAccess access;
    access.wordBytes = repr.discriminantBytes;
    access.bitOffset = repr.discriminantBitOffset;
    access.bitWidth = repr.discriminantBits;
    return access;
}

// Reading one. Unsigned whatever the tag's type is: a constructor index is a count, and a one-bit
// tag read as a signed field would decode constructor 1 as -1.
LowerPtr<LowerValue> decodeBitTag(LowerContext& lower, LowerBlock& block,
                                         LowerPtr<LowerValue> word, TypePtr record,
                                         TypePtr tagType, StringId name) {
    auto access = bitTagAccess(lower.repr.of(record));
    auto bits = decodePackedBits(lower, block, word, access, false);

    return cast<false, false>(lower.lower, lower.to, block, lower.lower[bits],
                              lowerType(lower.global, tagType), name)->created().ptr - lower.lower;
}

// Writing one, which is a read-modify-write of the word and therefore preserves the payload sharing
// it - the same property that lets two co-packed fields be written independently.
void encodeBitTag(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> word,
                         TypePtr record, U64 constructor) {
    auto access = bitTagAccess(lower.repr.of(record));
    encodePackedField(lower, block, word, access, immediate(lower, constructor));
}
