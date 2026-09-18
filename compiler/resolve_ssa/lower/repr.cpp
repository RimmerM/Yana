#include "repr.h"
#include "manifest.h"

namespace ssa {

/*
 * The target, read off the settings in one place.
 *
 * Every architecture this compiler emits for is little-endian, so today the byte order is a constant
 * wearing a switch. It is written as the switch anyway: the cost of naming the assumption is one
 * line, and the cost of not naming it is that the first big-endian target has to find every site
 * that wrote a number into bytes.
 */
NativeTarget nativeTargetFor(const CompileSettings& settings) {
    NativeTarget target;
    target.integers = targetIntWidths(settings);

    // The address and the word from one setting - `targetAddressBytes` - so that `Size` and a
    // pointer are the same width by construction rather than by two numbers agreeing.
    target.pointerSize = targetAddressBytes(settings);
    target.pointerAlign = target.pointerSize;

    switch(settings.arch) {
        case TargetArch::X64:
        case TargetArch::X86:
        case TargetArch::ARM:
        case TargetArch::ARM64:
            target.byteOrder = LittleEndian;
            break;
    }

    /*
     * A 32-bit address fills its word, so there is nowhere in a pointer to carry a shift - and a
     * narrow reference is the whole of what a `&` to a co-packed field is. Co-packing is switched
     * off rather than the reference refused, because the reference is taken in a generic body that
     * cannot see whether its subject's field was packed: the layout has to be the one every access
     * path can serve.
     */
    if(target.pointerSize < 8) {
        target.addressBits = target.pointerSize * 8;
        target.packFields = false;

        // And a word is what a load takes apart, so a scalarized record and a bit-tagged sum are
        // budgeted against the register this machine has rather than the widest one the language
        // allows - `kMaxPackBits` is the ceiling, and this is the target under it.
        target.maxPackBits = target.pointerSize * 8;
    }

    return target;
}

// Both halves in one place - see `Layout`.
Layout layoutFor(Context& context, mir::Program& program) {
    return Layout { *program.decl, nativeTargetFor(context.settings), &context, &program };
}

/*
 * A written integer type's width on this machine.
 *
 * The switch `intrinsic::TargetWidth` exists to defer, and the one place it is answered - so that
 * "how wide is a `Size`" has a single definition rather than one per site that lays a value out. A
 * fixed width answers with what it states, which is why every caller may ask this rather than
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
U32 storageBytesFor(U16 bits) {
    return mir::storageBitsFor(U8(bits > 64 ? 64 : bits)) / 8;
}

// Rounding up to a power-of-two boundary, in whatever unit the caller is counting - bytes for a
// field's offset, bits for a co-packed one's. One function because it is one piece of arithmetic and
// two spellings of it is how a placement comes to disagree with the size beside it.
static U32 alignTo(U32 value, U32 unit) {
    return unit ? (value + unit - 1) & ~(unit - 1) : value;
}

/*
 * The smallest number of bits whose patterns reach `highest`.
 *
 * The bits the *numbers* need, which is not a storage width: `Bool` is one bit in one byte, and that
 * one fact is what makes `Maybe(Bool)` a byte on a target that folds. Asked by the enum layout and
 * by nothing else, because everywhere else the question is about a type's storage rather than about
 * a count.
 */
static U32 bitsUpTo(U64 highest) {
    U32 bits = 1;
    while(bits < 64 && (U64(1) << bits) <= highest) bits++;
    return bits;
}

/*
 * How many patterns lie above the valid range, saturating.
 *
 * A word whose valid range is empty has 2^64 free patterns and no count can hold that; every caller
 * only ever compares the answer against a constructor count, so the largest number a `U64` holds is
 * as good as the true one and does not wrap on the way there.
 */
U64 Niche::freeAbove() const {
    auto width = U64(bytes) * 8;
    if(width >= 64) return validEnd == ~U64(0) ? 0 : ~U64(0) - validEnd;

    auto top = (U64(1) << width) - 1;
    return validEnd >= top ? 0 : top - validEnd;
}

U64 NicheEncoding::patternOf(U16 constructor) const {
    // The payload constructor has no pattern of its own - it is recognized by the scrutinee being
    // inside the valid range - so the others are numbered around it.
    auto index = U64(constructor > payloadConstructor ? constructor - 1 : constructor);
    return ascending ? firstPattern + index : firstPattern - index;
}

/*
 * A type this target has no layout for, because it is not concrete - a type parameter, and anything
 * measured over one.
 *
 * A named answer rather than a field set at each site, because `opaque` is the one thing about a
 * `Repr` a consumer may not ignore: `size == 0` is a real answer that `{}` gives, and the two lower
 * completely differently.
 */
static Repr opaqueRepr() {
    Repr repr;
    repr.align = 0;
    repr.opaque = true;
    return repr;
}

// A value that owns `used` of the `bytes` bytes it is stored in, counting up from zero - the shape a
// `@bits(n)` integer, a `WideInt` and a bounded enum discriminant all have.
static Niche lowRangeNiche(U32 bytes, U32 used, U64 highest) {
    if(!bytes) return Niche {};

    auto width = U32(bytes) * 8;
    if(used >= width) return Niche {};

    return Niche { 0, U8(bytes), 0, highest };
}

// An address that is never null: pattern zero is free and nothing else is. The one niche that needs
// no refinement to exist, and the reason `Maybe(&T)` is a plain nullable pointer.
static Niche addressNiche(U32 bytes) {
    return Niche { 0, U8(bytes), 1, ~U64(0) };
}

/*
 * The bits a value of this type owns and the unit it is allocated in, for the packing walk.
 *
 * `unitBits` is where the C bit-field rule lives: **no field straddles the natural storage unit of
 * its own width**, which is here for a reason C did not have - a `&` of a packed field is an address
 * plus a shift, and the *width of the load* that shift applies to has to be recoverable from the
 * field's type alone, because a callee holding one was compiled once and has only the type.
 * Guaranteeing the field sits inside one such unit is what makes that true.
 *
 * Under a `C` pin the unit is the **declared** type's width instead - `@bits(4) Int` is allocated in
 * an `int` the way `int x: 4` is - because matching a C compiler is the whole content of that
 * layout.
 */
static U32 declaredUnitBits(Layout layout, DeclPtr<mir::Type> type) {
    if(!mir::isRefinedInt(layout.base, type)) return 0;

    auto canonical = mir::canonicalInt(layout.base, type);
    if(layout.base[canonical]->kind != mir::Type::Int) return 0;

    return mir::storageBitsFor(U8(intBitsOn(layout.target, *(mir::IntType*)layout.base[canonical])));
}

/*
 * Whether a value of this type may be co-packed at all, before asking whether it is narrow enough.
 *
 * A type whose lifetime or whose copy is a *call* may not: every one of those calls takes the
 * address of the value, and a field that shares a word has no address of its own to give. Handing
 * over the word's would run the operation over the neighbour's bits. So an authored `Copy`, `Drop`
 * or `Reclaim` anywhere inside a type keeps it out of a shared word, and costs it the byte it would
 * have saved.
 */
static bool packableValue(Layout layout, DeclPtr<mir::Type> type) {
    auto ownership = mir::ownershipOf(layout.base, type);
    return ownership.trivialCopy && !ownership.isLinear();
}

/*
 * The source-level narrow fields of `@layout(packed)` - the pin is a wire format, so it must not
 * make the outer layout depend on whether an unpinned nested record happened to scalarize on this
 * target. Nested records remain ordinary fields; their own declaration decides whether they pack.
 */
static bool packedNarrowType(Layout layout, DeclPtr<mir::Type> type) {
    auto declared = layout.base[type];
    if(declared->kind == mir::Type::Int) return mir::isRefinedInt(layout.base, type);
    return declared->kind == mir::Type::Record &&
           ((mir::RecordType*)declared)->layout == mir::RecordLayout::Enum;
}

namespace {

/*
 * One field of a record as the layout walk sees it.
 *
 * Built once per record rather than per walk, because a record is walked several times - once per
 * constructor, and again by the scalar form - and a field's bytes are decided by its *contract* as
 * much as by its type. Rebuilding the occurrence at each walk is how two walks come to disagree
 * about which.
 */
struct Member {
    mir::Occurrence held;
    U32 size = 0;
    U32 align = 1;

    // The bits the value owns, and the unit they have to sit inside. Both zero for anything that
    // fills its own storage, which is what `packable` reads.
    U32 valueBits = 0;
    U32 unitBits = 0;

    // Narrow, and a value this target may move in and out of a shared word.
    bool narrow = false;
    bool packable = false;

    Niche niche;
};

using Members = SmallArray<Member, 8>;

/*
 * The two lists the packing walk works in, both inline.
 *
 * A record with more than sixteen narrow fields is not one this bound is deciding anything about;
 * the point is that the ordinary record - a handful of fields, laid out once per declaration - never
 * reaches the heap for either of them.
 */
using PackOrder = SmallArray<U16, 16>;
using PackOffsets = SmallArray<U32, 16>;

// Where a run of co-packed fields ended up. `span` is the bits it occupies including any gap the
// straddle rule left behind, since what a container needs to know is how wide the word has to be
// rather than how much of it is live; `count` is how many of the fields offered were placed.
struct PackedRun {
    U32 span = 0;
    U16 count = 0;
};

// One constructor's fields, placed from zero over the storage its arm shares with the others.
struct ArmLayout {
    U32 size = 0;
    U32 align = 1;
    Niche niche;
};

} // namespace

static Members membersOf(Layout layout, mir::RecordType& record,
                         DeclPtr<mir::OwnershipSchema> shape) {
    auto base = layout.base;
    Members members;

    for(auto field: record.fields.contents(base)) {
        /*
         * The field's own contract, not `owned`: an external field holds a reference to somebody
         * else's object and is a pointer, and asking for the owned answer would lay out the object.
         * Or the **occurrence's**, where a qualified type argument said the position is somebody
         * else's - `fieldContract`, which is the one place the two sources are read in order. That
         * is the whole physical consequence of §2.4: the logical type is unchanged and the bytes
         * are not.
         */
        mir::Occurrence held { field.occurrence.type, mir::fieldContract(base, shape, field),
                               nullptr };

        Member member;
        member.held = held;

        /*
         * A **boxed** field is a pointer, and this arm is where the whole layout consequence of
         * `RecordField::boxed` lives - `doc/spec/repr.md`'s `@box`.
         *
         * It is taken **before** anything asks what the target's layout is, and that order is not a
         * micro-optimization: it is what makes a recursive declaration lay out at all. `data List =
         * Nil | Cons {tail: List}` reaches here with `tail` boxed by `breakLayoutCycles`, and a
         * `manifestNative` of `List` on the way to deciding that would be `reprOf(List)` from inside
         * `reprOf(List)`. The box is the base case of that recursion, so it cannot be reached
         * through one.
         *
         * Never packable and never scalarized: a pointer is a whole word on every target this stage
         * emits, so there is no narrow value to share a word with. `@bits` on a boxed field does not
         * change that - the refinement applies to the value on the far side of the pointer, and the
         * slot holding the pointer is a word either way.
         *
         * It donates the **address niche** unconditionally, where a borrow donates one only where
         * the target promises non-null borrows. The difference is who wrote the pointer: a box is
         * the compiler's own allocation, filled by the construction that made the value and never by
         * anything a program can spell, so nothing can put a zero in it. That is what makes
         * `Maybe(Tree)` cost one word - `Nothing` is the null the payload cannot be.
         */
        if(mir::indirectField(base, field)) {
            member.size = layout.target.pointerSize;
            member.align = layout.target.pointerSize;
            member.niche = addressNiche(layout.target.pointerSize);
            members.push(member);
            continue;
        }

        auto manifest = manifestNative(layout, held);

        member.size = manifest.size;
        member.align = manifest.align ? manifest.align : 1;

        /*
         * A field holding a **loan** is an address, and a published borrow is never null - so it
         * donates the address niche whatever it points at, and shares a word with nothing.
         *
         * A **raw pointer** donates nothing, and the exclusion is the language's rather than the
         * machine's: `null()` and `isNull` are ordinary Native intrinsics that the allocator is
         * written in terms of, so zero is a pattern a `%T` legally holds. It reaches the row below
         * as a type whose own niche is empty.
         */
        if(isReferenceValue(layout, held)) {
            member.niche = layout.target.nonNullBorrows ? addressNiche(layout.target.pointerSize)
                                                        : Niche {};
        } else {
            auto content = reprOf(layout, held.type);
            member.niche = content.niche;

            member.narrow = content.isNarrow() &&
                            (record.pin != mir::TypeLayout::Packed ||
                             packedNarrowType(layout, held.type));
            if(member.narrow && packableValue(layout, held.type)) {
                member.valueBits = content.scalarBits;
                member.unitBits = record.pin == mir::TypeLayout::C
                                ? declaredUnitBits(layout, held.type) : 0;
                if(record.pin != mir::TypeLayout::Packed && !member.unitBits) {
                    member.unitBits = mir::storageBitsFor(U8(member.valueBits));
                }
                member.packable = true;
            }
        }

        members.push(member);
    }

    return members;
}

/*
 * Whether this target may co-pack field `index` with another of the same arm.
 *
 * Narrow, *and* sharing the arm with something else narrow. The second half is what keeps a record
 * with one `Bool` in it addressable: co-packing a lone field with nothing saves no space and would
 * cost every borrow of it a temporary, so a field only gives up its address where the declaration is
 * actually asking to be packed.
 *
 * "Sharing the arm with" rather than "written next to", because an `Auto` layout is free to reorder
 * the fields and does - `{a: Bool, b: U64, c: Bool}` packs `a` with `c`. Under `C` the order *is*
 * the declaration's, so there the neighbours are the written ones, exactly as a C bit-field's are.
 */
static bool packCandidate(Layout layout, mir::RecordType& record, const Members& members, U16 first,
                          U16 count, U16 index) {
    if(!layout.target.packFields) return false;
    if(!members[index].packable) return false;

    // A `@layout(js)` record keeps one property per field, which is the whole content of the pin,
    // so nothing in it shares with anything.
    if(record.pin == mir::TypeLayout::Js) return false;

    /*
     * Under `C` only a written refinement is a bit-field - `@bits(4) Int` is `int x: 4`, and a
     * `Bool` is a whole `_Bool` rather than one bit of a byte. That distinction does not exist under
     * `Auto`, where every narrow value is a candidate and a `Bool` costing a bit rather than a byte
     * is the point; it exists here because a C header has both spellings and they lay out
     * differently.
     */
    if(record.pin == mir::TypeLayout::C) {
        auto bitField = [&](U16 at) {
            return members[at].packable && mir::isRefinedInt(layout.base, members[at].held.type);
        };

        if(!bitField(index)) return false;
        return (index > first && bitField(U16(index - 1))) ||
               (U16(index + 1) < U16(first + count) && bitField(U16(index + 1)));
    }

    for(U16 at = first; at < U16(first + count); at++) {
        if(at != index && members[at].packable) return true;
    }

    return false;
}

/*
 * The bit placement of a run of narrow fields - the one rule the layout and every access agree on.
 *
 * `order` is the field indices to try, from `from`, in the order they should be placed. The run
 * stops at the first field that does not fit the budget, and the caller places the rest another way.
 */
static PackedRun packBits(const Members& members, const PackOrder& order, Size from, U32 budget,
                          PackOffsets* offsets) {
    PackedRun run;

    for(auto at = from; at < order.size(); at++) {
        auto& member = members[order[at]];
        if(!member.valueBits) break;

        auto placed = run.span;

        // Bumped to the next unit boundary where it would otherwise cross one. Measured before the
        // budget check, since the bump is what decides whether the field still fits.
        auto unit = member.unitBits;
        if(unit && placed / unit != (placed + member.valueBits - 1) / unit) {
            placed = alignTo(placed, unit);
        }

        if(placed + member.valueBits > budget) break;

        if(offsets) offsets->push(placed);
        run.span = placed + member.valueBits;
        run.count++;
    }

    return run;
}

/*
 * The order narrow fields are packed in: widest first, and stable within a width.
 *
 * Widest first because the straddle rule wastes the *tail* of a unit rather than its head, so
 * placing the wide fields while the word is still empty leaves the leftover bits in one run at the
 * top where a niche can use them. `{d: @bits(4), f: @bits(4), a: Bool}` fills a byte with `d` and
 * `f` and starts a second one with `a`, where declaration order would split `f` across the two.
 *
 * Declaration order under a pin, which is what pinning the layout means.
 */
static void packOrder(const Members& members, PackOrder& into, bool pinned) {
    if(pinned) return;

    for(Size i = 1; i < into.size(); i++) {
        auto index = into[i];
        auto width = members[index].valueBits;
        auto at = i;

        while(at > 0 && members[into[at - 1]].valueBits < width) {
            into[at] = into[at - 1];
            at--;
        }

        into[at] = index;
    }
}

/*
 * One constructor's fields, placed from zero.
 *
 * The size, the alignment, every field's offset and the niche come out of the same accumulation,
 * because they are one answer: a size computed separately from the offsets is a size that can
 * disagree with them, and the disagreement is a write past the end of an allocation.
 *
 * Two groups, and that is the whole shape of the walk. Everything that owns its storage goes first,
 * widest alignment first and then widest size, which is the ordinary way to spend padding: a `U64`
 * after a `Bool` costs seven bytes of hole, and the same two fields the other way round cost none.
 * Then the pack candidates, in `packOrder`, because a run of bit-fields wants to end up in one word
 * and a word placed last can be as narrow as what is left over rather than as wide as the alignment
 * it landed on. A pin is exactly what opts out of both.
 *
 * `placed` is indexed by the record's own field numbering, which is what lets the arms of a sum
 * share one list: a field index identifies its constructor as well, so two arms never write the same
 * entry even though they name overlapping bytes.
 */
static ArmLayout placeArm(Layout layout, mir::RecordType& record, const Members& members, U16 first,
                          U16 count, FieldRepr* placed) {
    ArmLayout arm;
    if(!count) return arm;

    PackOrder plain, packed;
    for(U16 i = first; i < U16(first + count); i++) {
        if(packCandidate(layout, record, members, first, count, i)) packed.push(i);
        else plain.push(i);
    }

    // Insertion sort, and stable, so that two fields a target has no reason to distinguish stay in
    // the order they were declared - the layout dump of a record is easier to read against its
    // declaration that way, and it is one fewer thing that changes when a field is added.
    if(!record.pinned()) {
        for(Size i = 1; i < plain.size(); i++) {
            auto index = plain[i];
            auto& member = members[index];
            auto at = i;

            while(at > 0) {
                auto& previous = members[plain[at - 1]];
                if(previous.align > member.align) break;
                if(previous.align == member.align && previous.size >= member.size) break;

                plain[at] = plain[at - 1];
                at--;
            }

            plain[at] = index;
        }
    }

    U32 size = 0;
    U32 align = 1;

    /*
     * The niche this arm republishes - **the first one any placed component exposes**, in placement
     * order, with the offset adjusted to where that component sits.
     *
     * First rather than best, which is the shipping compiler's rule too. What a niche is worth is
     * how many patterns it leaves free, and the sum that will spend it has one constructor count -
     * so a search comparing candidates would be optimizing for a number this walk does not have.
     * What matters is that a record *containing* a niche-bearing member exposes one at all, which is
     * what makes folding recursive rather than one level deep.
     */
    auto publish = [&](const Niche& found, U32 offset) {
        if(arm.niche.exists() || !found.exists()) return;
        arm.niche = found;
        arm.niche.offset += offset;
    };

    for(auto index: plain) {
        auto& member = members[index];
        auto memberSize = member.size;
        auto memberAlign = member.align;

        /*
         * A bit-field that shares its unit with nobody still occupies the whole unit under a pinned
         * layout, because C says so: `int d: 4` between two other members takes four bytes, not the
         * one its four bits would fit in. Only the *slot* widens - reading and writing it is still a
         * load of the value's own width at its own address, which on this byte order is the low end
         * of the unit.
         */
        if(record.pin == mir::TypeLayout::C && member.unitBits) {
            memberSize = max(memberSize, member.unitBits / 8);
            memberAlign = max(memberAlign, member.unitBits / 8);
        }

        size = alignTo(size, memberAlign);

        placed[index] = FieldRepr { member.held, size, U8(memberSize > 255 ? 0 : memberSize), 0, 0 };
        publish(member.niche, size);

        size += memberSize;
        align = max(align, memberAlign);
    }

    packOrder(members, packed, record.pinned());
    auto budget = min(layout.target.maxPackBits, kMaxPackBits);

    Size at = 0;
    while(at < packed.size()) {
        PackOffsets offsets;
        auto run = packBits(members, packed, at, budget, &offsets);

        /*
         * A field alone in a word is not packed. It keeps its natural storage and therefore its
         * address, which costs nothing in space and saves every borrow of it a temporary - Design.md's
         * tier 0. This is reached at the tail of a run too long for one word.
         */
        if(run.count < 2) {
            auto index = packed[at++];
            auto& member = members[index];

            size = alignTo(size, member.align);
            placed[index] = FieldRepr { member.held, size, U8(member.size), 0, 0 };
            publish(member.niche, size);

            size += member.size;
            align = max(align, member.align);
            continue;
        }

        /*
         * How wide the word is. As narrow as the bits need, except under a pinned layout, where it
         * is as wide as the declared type of the widest bit-field in the run - two `int x: 4` share
         * four bytes in C rather than one, and the whole point of the pin is that they do here too.
         */
        auto wordBytes = U32(mir::storageBitsFor(U8(run.span)) / 8);
        if(record.pin == mir::TypeLayout::C) {
            U32 unit = 0;
            for(U16 i = 0; i < run.count; i++) unit = max(unit, members[packed[at + i]].unitBits);
            if(unit) wordBytes = alignTo(run.span, unit) / 8;
        }

        size = alignTo(size, wordBytes);
        auto offset = size;

        size += wordBytes;
        align = max(align, wordBytes);

        for(U16 i = 0; i < run.count; i++) {
            auto index = packed[at + i];
            placed[index] = FieldRepr { members[index].held, offset, U8(wordBytes), U8(offsets[i]),
                                        U8(members[index].valueBits) };
        }

        /*
         * What the word has left over, which is a better niche than any of these fields could have
         * donated alone: two booleans in one byte leave 252 patterns free where each of them
         * separately left the high half of its own word.
         *
         * A packed field never republishes its own niche - there is no whole word for it to be a
         * niche *of* - so this is the only way a packed run contributes one.
         */
        if(run.span < wordBytes * 8) {
            publish(Niche { 0, U8(wordBytes), 0, lowMask(run.span) }, offset);
        }

        at += run.count;
    }

    arm.size = size;
    arm.align = align;
    return arm;
}

/*
 * One `@layout(packed)` record, in declaration order.
 *
 * The cursor is in bits, but ordinary fields may only begin at a byte boundary. A narrow field is
 * the exception: it spends exactly its logical value bits, and the next field continues immediately
 * after it. That makes a partial byte an error rather than an opportunity for the compiler to add
 * padding. Narrow fields may cross both byte boundaries and the natural units of their source type;
 * the access is chosen after the record size is known, so it can use one legal load/store word or
 * exact byte-wise read-modify-writes when no single word covers the field. `Bool` and a payload-free
 * enum arrive here as narrow values too, so they can deliberately complete a byte beside written
 * `@bits` fields.
 *
 * The existing packed access path loads one 1/2/4/8-byte word where possible. A zero `wordBytes`
 * records the fallback; `pack.cpp` then touches exactly the bytes in the field's range, which keeps
 * a five-byte field from overreading or overwriting a six-byte record merely to obtain an eight-byte
 * machine access.
 */
static bool layoutPackedRecord(Layout layout, mir::RecordType& record, const Members& members,
                               FieldRepr* placed, Repr& into) {
    auto base = layout.base;
    U32 bitCursor = 0;

    auto report = [&](StringView message, LocationId source) {
        layout.context->diagnostics.error(message, source);
    };

    for(U16 index = 0; index < record.fields.size(); index++) {
        auto& member = members[index];

        if(member.narrow) {
            if(!member.packable) {
                report("a narrow field in a `@layout(packed)` record cannot be represented without an addressable storage unit"_v,
                       record.fields.get(base, index).source);
                return false;
            }

            auto width = member.valueBits;
            placed[index] = FieldRepr { member.held, bitCursor / 8, 0, U8(bitCursor % 8),
                                        U8(width) };
            bitCursor += width;
            continue;
        }

        if(bitCursor % 8) {
            report("a `@layout(packed)` record cannot leave a partial byte before an ordinary field"_v,
                   record.fields.get(base, index).source);
            return false;
        }

        auto offset = bitCursor / 8;
        placed[index] = FieldRepr { member.held, offset,
                                    U8(member.size > 255 ? 0 : member.size), 0, 0 };
        bitCursor += member.size * 8;
    }

    if(bitCursor % 8) {
        report("a `@layout(packed)` record must fill its final byte with bit-fields"_v, record.source);
        return false;
    }

    /*
     * Pick a single access word where one exists. It may begin in an earlier byte than the field;
     * that is useful for a field such as forty bits at bit twelve in an eight-byte page-table entry,
     * where an eight-byte access from byte zero is exact but an access from byte one would overread.
     * The fallback leaves the field at its first byte and lets `pack.cpp` split it into byte accesses.
     */
    auto size = bitCursor / 8;
    static constexpr U32 kAccessBytes[] = { 1, 2, 4, 8 };
    for(U16 index = 0; index < record.fields.size(); index++) {
        auto& field = placed[index];
        if(!field.isPacked()) continue;

        auto firstByte = field.offset;
        auto firstBit = U32(field.bitOffset);
        bool found = false;

        for(auto wordBytes: kAccessBytes) {
            auto earliest = firstByte >= wordBytes - 1 ? firstByte - (wordBytes - 1) : 0;
            for(auto wordOffset = firstByte;; wordOffset--) {
                auto bitOffset = firstBit + (firstByte - wordOffset) * 8;
                if(bitOffset + field.bitWidth <= wordBytes * 8 &&
                   wordOffset + wordBytes <= size) {
                    field.offset = wordOffset;
                    field.wordBytes = U8(wordBytes);
                    field.bitOffset = U8(bitOffset);
                    found = true;
                    break;
                }
                if(wordOffset == earliest) break;
            }
            if(found) break;
        }
    }

    into.size = bitCursor / 8;
    into.align = 1;
    into.stride = into.size;
    return true;
}

/*
 * The whole record as one integer - `NativeTarget::scalarizeRecords`.
 *
 * The same placement `placeArm` applies to a run inside a record, applied to the record itself, and
 * that identity is the point: `data Flags {a: Bool, b: Bool}` is two bits whether it is reached on
 * its own or as a field of something else, so a container may co-pack it and a `&` of it carries a
 * shift.
 *
 * All of it or none of it. A scalar record *is* its fields' bits, so one field that fills its own
 * storage is the whole record laid out the ordinary way - which is what keeps `{a: U8, b: U8}` two
 * bytes rather than two bit-fields of a word.
 */
static bool scalarizeRecord(Layout layout, mir::RecordType& record, const Members& members,
                            Repr& into, FieldRepr* placed) {
    if(!layout.target.scalarizeRecords) return false;

    /*
     * A pinned layout has no scalar form, and that is the pin's own statement rather than a
     * limitation: its whole purpose is that its fields sit where a C compiler put them, and a scalar
     * is the compiler choosing. `doc/spec/repr.md` puts a pin in a tier of its own for exactly this.
     */
    if(record.pinned()) return false;

    auto count = U16(record.fields.size());
    if(!count || record.cons.size() != 1) return false;

    PackOrder order;
    for(U16 i = 0; i < count; i++) {
        if(!members[i].packable) return false;
        order.push(i);
    }

    packOrder(members, order, false);

    PackOffsets offsets;
    auto run = packBits(members, order, 0, min(layout.target.maxPackBits, kMaxPackBits), &offsets);
    if(run.count != count) return false;

    // A run that exactly fills its storage is not a *narrow* scalar - it has no bits left to be
    // packed into a neighbour with, so calling it one would only cost every borrow of it a shift it
    // does not need, and would claim a niche the fields can write at will.
    auto bytes = U32(mir::storageBitsFor(U8(run.span)) / 8);
    if(run.span >= bytes * 8) return false;

    into.size = bytes;
    into.align = bytes;
    into.stride = bytes;
    into.scalarBits = run.span;

    for(U16 i = 0; i < run.count; i++) {
        auto index = order[i];
        placed[index] = FieldRepr { members[index].held, 0, U8(bytes), 0, 0 };

        /*
         * Marked packed only where the field would have shared the word with somebody.
         *
         * A record with one field covering it - `data Wrapped {value: @bits(53) U64}` - is the same
         * bytes either way, so the only difference packing it would make is to take away the address
         * `addressOf` is still allowed to hand out. `packCandidate` already declines a lone field
         * for that reason; reading it here is what keeps the two answers one answer.
         */
        if(packCandidate(layout, record, members, 0, count, index)) {
            placed[index].bitOffset = U8(offsets[i]);
            placed[index].bitWidth = U8(members[index].valueBits);
        }
    }

    // Everything above the span, which is the niche `Maybe(Flags)` folds into - and the reason a
    // scalar record is worth having even where it saved no space.
    into.niche = Niche { 0, U8(bytes), 0, lowMask(run.span) };
    return true;
}

/*
 * Design.md's "Niches and automatic packing", and the reason it is a search rather than a list of
 * special cases.
 *
 * Before allocating a discriminant word, ask whether some constructor's payload already has patterns
 * it cannot produce. If one does and there are enough of them for every *other* constructor, the
 * record costs exactly that payload: the niche-bearing constructor is the payload itself, and each
 * of the others is one impossible pattern. Nothing here knows what kind of type supplied the niche,
 * which is the property that makes `Maybe(&T)`, `Maybe(Id)` and `Maybe` of a library type with a
 * declared niche all work with nothing written against `Maybe`.
 *
 * The patterns stand for **positions** and not for the numbers the constructors carry, which is what
 * lets a `@value(n)`-pinned sum fold at all: `patternOf` counts away from the first pattern in
 * declaration order, and the read turns the position it recovers back into the number. Those are the
 * same thing for an unpinned sum and are not for a pinned one, and keeping them apart here is what
 * makes the pin a question for the *decode* rather than a refusal - see `decodeFolded`.
 */
static bool foldNiche(Layout layout, mir::RecordType& record, const SmallArray<ArmLayout, 4>& arms,
                      Repr& into) {
    if(!layout.target.foldNiches) return false;

    // A pin freezes the discriminant with everything else: a stored tag in front of the payload is
    // what a C reader expects, and folding is the compiler choosing.
    if(record.pinned()) return false;

    auto base = layout.base;
    auto constructors = record.cons.size();
    auto others = U64(constructors - 1);
    if(!others) return false;

    for(U16 i = 0; i < constructors; i++) {
        auto constructor = record.cons.get(base, i);
        if(!constructor.count) continue;
        if(!arms[i].niche.fits(others)) continue;

        // Every other constructor has to fit *inside* this payload, since the folded record is
        // exactly this payload and there is nowhere else for their fields to go. In practice that
        // means they carry nothing, which is the `Nothing`/`Just(a)` shape this exists for.
        auto usable = true;
        for(U16 other = 0; other < constructors; other++) {
            if(other != i && record.cons.get(base, other).count) { usable = false; break; }
        }

        if(!usable) continue;

        into.size = arms[i].size;
        into.align = arms[i].align;
        into.discriminant = Discriminant::Niche;
        into.discriminantOffset = 0;
        into.discriminantBytes = 0;
        into.payloadOffset = 0;

        auto& encoding = into.encoding;
        encoding.niche = arms[i].niche;
        encoding.payloadConstructor = i;

        // Prefer the patterns below the valid range, so that a `Maybe` over a non-null address gets
        // `Nothing == 0` and becomes a plain nullable pointer.
        if(encoding.niche.freeBelow() >= others) {
            encoding.firstPattern = encoding.niche.validStart - 1;
            encoding.ascending = false;
        } else {
            encoding.firstPattern = encoding.niche.validEnd + 1;
            encoding.ascending = true;
        }

        // The folded record has consumed the patterns it needed; whatever is left over is still a
        // niche for whatever contains *this* record in turn.
        auto remaining = encoding.niche;
        if(encoding.ascending) remaining.validEnd = encoding.firstPattern + others - 1;
        else remaining.validStart = encoding.firstPattern - (others - 1);

        into.niche = remaining.freeBelow() || remaining.freeAbove() ? remaining : Niche {};
        return true;
    }

    return false;
}

/*
 * A sum as one integer, with the tag in the bits its arms do not use - `NativeTarget::bitTagSums`.
 *
 * The case folding cannot reach and a tag word overpays for. `data Pick = A(x: Bool) | B(y: Bool)`
 * has a niche in either arm and folding declines, because folding puts *every other* constructor
 * outside the payload's range and both of these carry one. What is true instead is that neither arm
 * spends more than a bit, so a tag placed above them costs the second bit of a byte rather than a
 * byte of its own and the alignment behind it.
 *
 * ## Why every arm has to pack, and why the tag then sits above the widest of them
 *
 * The shipping compiler puts its tag above the payload's whole *storage*, padding included, so that
 * a field written at its own width can never reach it. That is the right rule for a four-byte tag,
 * where the saving is enormous whatever it costs in density. It is worth nothing here: this stage's
 * tag is already a byte, so `alignTo(1, payloadAlign) + payloadBytes` is never more than the next
 * power of two up from `payloadBytes * 8 + 1` bits, and the bit form would tie at best.
 *
 * So the tag goes above the bits the arms actually *spend*, and what makes that safe is the
 * condition rather than the placement: every field of every arm is co-packed, so every write to one
 * is a read-modify-write that preserves everything outside its own range - which is exactly what a
 * neighbouring tag needs. An arm with one field is packed here where `packCandidate` would have left
 * it addressable, and that is not an exception: the tag *is* the neighbour it shares the word with.
 *
 * The result is a **scalar**, which is the second half of what this buys. A bit-tagged sum has
 * `scalarBits`, so it co-packs into a parent, folds into a `Maybe` and is borrowed as a bit range
 * like any other narrow value - and the tag it reads through that borrow is at the shift the
 * reference carried plus `discriminantBitOffset`, which is the same composition a field of a
 * scalarized record gets.
 */
static bool scalarizeSum(Layout layout, mir::RecordType& record, const Members& members, Repr& into,
                         FieldRepr* placed, U32 tagBytes, U32 payloadBytes, U32 payloadAlign) {
    if(!layout.target.bitTagSums) return false;

    // A pin freezes the discriminant with everything else - see `foldNiche`, which declines for the
    // same reason: a tag in front of the payload is what a C reader expects.
    if(record.pinned()) return false;

    auto base = layout.base;
    auto range = mir::tagRangeOf(base, record);

    // A tag that reaches below zero is not a low run of patterns, and a sign bit is not something a
    // few bits above a payload can carry - the same refusal `layoutEnum` makes about its niche.
    if(range.isSigned || range.highest < 0) return false;

    auto tagBits = bitsUpTo(U64(range.highest));
    auto budget = min(layout.target.maxPackBits, kMaxPackBits);
    if(tagBits >= budget) return false;

    /*
     * Every arm, packed from zero over the storage they share - the same `packBits` a run inside a
     * record gets, at a budget one tag short.
     *
     * Staged rather than written straight into `placed`, because this walk can decline *after* it
     * has placed everything - the size comparison at the end is the last thing it learns - and the
     * word-tagged form below is entitled to find the placement `placeArm` already made. A pass that
     * left its working state behind on the way out marked a lone field of a sum's arm packed and
     * then laid the record out with a tag word in front of it.
     */
    SmallArray<FieldRepr, 8> staged;
    for(U16 i = 0; i < record.fields.size(); i++) staged.push(placed[i]);

    U32 payloadBits = 0;

    for(U16 i = 0; i < record.cons.size(); i++) {
        auto constructor = record.cons.get(base, i);
        if(!constructor.count) continue;

        PackOrder order;
        for(U16 f = constructor.first; f < U16(constructor.first + constructor.count); f++) {
            if(!members[f].packable) return false;
            order.push(f);
        }

        packOrder(members, order, false);

        PackOffsets offsets;
        auto run = packBits(members, order, 0, budget - tagBits, &offsets);
        if(run.count != constructor.count) return false;

        for(U16 k = 0; k < run.count; k++) {
            auto index = order[k];
            staged[index] = FieldRepr { members[index].held, 0, 0, U8(offsets[k]),
                                        U8(members[index].valueBits) };
        }

        payloadBits = max(payloadBits, run.span);
    }

    auto span = payloadBits + tagBits;
    auto bytes = U32(mir::storageBitsFor(U8(span)) / 8);
    auto align = max(bytes, payloadAlign);

    /*
     * What the word-tagged form below would have cost, and this only happens if it is genuinely
     * smaller. A bit tag is a shift and a mask where a tag word is a load, so where the two occupy
     * the same bytes the word is the better answer.
     */
    auto wordAlign = max(tagBytes, payloadAlign);
    auto wordSize = alignTo(alignTo(tagBytes, payloadAlign) + payloadBytes, wordAlign);
    if(alignTo(bytes, align) >= wordSize) return false;

    into.size = alignTo(bytes, align);
    into.align = align;
    into.stride = into.size;
    into.scalarBits = span;
    into.discriminant = Discriminant::Bits;
    into.discriminantOffset = 0;
    into.discriminantBytes = bytes;
    into.discriminantBitOffset = payloadBits;
    into.discriminantBits = tagBits;
    into.payloadOffset = 0;

    for(U16 i = 0; i < record.fields.size(); i++) {
        placed[i] = staged[i];
        placed[i].wordBytes = U8(bytes);
    }

    // Everything above the tag, on the same bargain a co-packed word's leftover bits are held to:
    // nothing writes those bits, so they are free only because the storage was zeroed - which is
    // what `hasPaddedWord` arranges.
    if(span < into.size * 8) into.niche = Niche { 0, U8(min(into.size, 8u)), 0, lowMask(span) };

    return true;
}

/*
 * The box provenance word, after every field - `Repr::boxFlagsOffset`.
 *
 * One bit per boxed field of the *whole* record, appended once the fields are placed and the size
 * is settled. Appending rather than interleaving is what keeps the two compilation modes comparable:
 * every offset above this line is the offset the record would have had with no word at all, so a
 * build that stores nothing and a build that stores a byte differ by a suffix and by nothing else.
 *
 * Nothing is done in the mode that stores no provenance, and nothing is done for a record with no
 * boxed field - which is almost every record, so almost every record pays one predictable test.
 *
 * The alignment is untouched. A byte of flags needs none, and raising a record's alignment for the
 * compiler's own bookkeeping would change the stride of every array of it.
 */
static void placeBoxFlags(Layout layout, mir::RecordType& record, Repr& into, FieldRepr* placed,
                          U16 count) {
    if(!storesBoxProvenance(layout)) return;

    U16 boxes = 0;
    for(U16 i = 0; i < count; i++) {
        if(mir::indirectField(layout.base, record.field(layout.base, i))) boxes++;
    }

    if(!boxes) return;

    /*
     * A record whose every field was scalarized into one integer cannot also carry a word, and it
     * never has to: co-packing takes only narrow values and a pointer is not one, so a record with a
     * boxed field is never scalarized. Asserted rather than handled, because the alternative is a
     * flags byte at an offset inside a value that has no bytes.
     */
    assertTrue(!into.scalarBits);

    into.boxFlagsOffset = into.size;
    into.boxFlagsBytes = U8((boxes + 7) / 8);
    into.size += into.boxFlagsBytes;
    into.stride = alignTo(into.size, into.align);

    U8 bit = 0;
    for(U16 i = 0; i < count; i++) {
        if(!mir::indirectField(layout.base, record.field(layout.base, i))) continue;
        placed[i].boxFlagBit = bit++;
    }

    /*
     * The niche the fields published still stands, and the word does not add one.
     *
     * A boxed field donates its address's null pattern and that pattern is still free - the bytes
     * this appended are behind it and change nothing about what the pointer can hold. What the word
     * must not do is *become* the niche: its unused bits are written by the construction that sets
     * the flags and read by nothing else, so a container folding a tag into them would be folding it
     * into storage another walk writes.
     */
}

/*
 * One record's layout, in one walk.
 *
 * Three shapes, and the order they are tried in is the order of decreasing information: a record
 * with one constructor may be one integer, a sum may be its payload with the tag folded into the
 * patterns that payload cannot produce, and what is left is the tag word in front of a shared
 * payload area.
 *
 * A prefix walk is the field count for one field and its square for all of them, and it stays that
 * way: the offsets of a record are the sizes and alignments of its members, so this walk is the one
 * arithmetic that produces them and a table of offsets would be a second answer to keep in step. The
 * term that grows is each member's own layout, which the memo in `reprOf` removes.
 */
static void layoutRecord(Layout layout, mir::RecordType& record, Repr& into,
                         DeclPtr<mir::OwnershipSchema> shape) {
    auto base = layout.base;
    auto& arena = layout.program->decl;

    auto members = membersOf(layout, record, shape);
    auto count = U16(record.fields.size());

    SmallArray<FieldRepr, 8> placed;
    for(U16 i = 0; i < count; i++) placed.push(FieldRepr { members[i].held, 0, 0, 0, 0 });

    if(record.pin == mir::TypeLayout::Packed) {
        if(record.layout == mir::RecordLayout::Multi) {
            // Resolution reports this with the declaration's source. Keep this guard for a record
            // reached through an incomplete/error path, so a data-bearing sum never gets the ordinary
            // tag-and-payload layout by accident.
            layout.context->diagnostics.error(
                "`@layout(packed)` cannot be used on a sum type whose constructors carry data"_v,
                record.source);
            into.opaque = true;
            return;
        }

        if(!layoutPackedRecord(layout, record, members, placed.pointer(), into)) {
            into.opaque = true;
            return;
        }

        placeBoxFlags(layout, record, into, placed.pointer(), count);
        into.fields.reserve(arena, count);
        for(U16 i = 0; i < count; i++) into.fields.push(arena, placed[i]);
        return;
    }

    auto tagBytes = U32(mir::tagBitsOf(base, record) / 8);

    if(!tagBytes) {
        // One constructor and no discriminant, which is every ordinary `data Pair {x: I32}`.
        if(!scalarizeRecord(layout, record, members, into, placed.pointer())) {
            auto constructor = record.cons.get(base, 0);
            auto arm = placeArm(layout, record, members, constructor.first, constructor.count,
                                placed.pointer());

            /*
             * **No tail padding**, which is the difference between a size and a stride.
             *
             * A record's trailing padding belongs to whoever contains it:
             * `data Pad {wide: I64, narrow: I8}` occupies nine bytes and two of them are sixteen
             * apart, so a `data Nest {inner: Pad, tag: I8}` puts its tag at *nine* and is ten bytes
             * long. Rounding here would make the number the array stride and lose the other one, and
             * there is no way back from that - the padding is not recoverable from a rounded size,
             * so every containing record would grow with it.
             */
            into.size = arm.size;
            into.align = arm.align;
            into.stride = alignTo(arm.size, arm.align);
            into.niche = arm.niche;
        }
    } else {
        /*
         * The payload, laid out **once per constructor over the same storage**.
         *
         * That is what makes a sum a sum: the arms are alternatives, so their fields start at one
         * payload offset and the record is as large as the widest of them. A field index identifies
         * its own constructor, which is why there is no downcast step in a place path and no second
         * projection to keep in step with this arithmetic.
         *
         * Each arm is measured from zero and the shared offset is added once at the end, because
         * that offset depends on the strongest alignment any arm needs and the arms are measured in
         * one pass.
         */
        SmallArray<ArmLayout, 4> arms;
        U32 payloadBytes = 0;
        U32 payloadAlign = 1;

        for(U16 i = 0; i < record.cons.size(); i++) {
            auto constructor = record.cons.get(base, i);
            auto arm = placeArm(layout, record, members, constructor.first, constructor.count,
                                placed.pointer());

            payloadBytes = max(payloadBytes, arm.size);
            payloadAlign = max(payloadAlign, arm.align);
            arms.push(arm);
        }

        if(foldNiche(layout, record, arms, into)) {
            // The payload *is* the record, so the arm-relative offsets the walk produced are already
            // the record's and there is nothing to shift.
            into.stride = alignTo(into.size, into.align);
        } else if(scalarizeSum(layout, record, members, into, placed.pointer(), tagBytes,
                               payloadBytes, payloadAlign)) {
            // Every field was placed again, as bits of the one word the tag is also in - so there is
            // likewise nothing to shift, and the arms this walk measured were only the comparison.
        } else {
            /*
             * The discriminant, in front of every payload - a decision rather than an observation:
             * what reads a tag is a match, and a match on a value whose payload it has proved
             * nothing about may only touch bytes every arm agrees are the tag. Putting it first
             * makes that the same address for every arm and for every instantiation.
             */
            auto payloadOffset = alignTo(tagBytes, payloadAlign);

            into.discriminant = Discriminant::Word;
            into.discriminantOffset = 0;
            into.discriminantBytes = tagBytes;
            into.discriminantBits = tagBytes * 8;
            into.payloadOffset = payloadOffset;
            into.align = max(tagBytes, payloadAlign);
            into.size = payloadOffset + payloadBytes;
            into.stride = alignTo(into.size, into.align);

            for(U16 i = 0; i < count; i++) placed[i].offset += payloadOffset;

            /*
             * A record whose own discriminant is stored publishes no niche: the tag word is not
             * free, and what lies under it is a different pattern per arm. The folded form above is
             * the one with something to say.
             */
        }
    }

    placeBoxFlags(layout, record, into, placed.pointer(), count);

    into.fields.reserve(arena, count);
    for(U16 i = 0; i < count; i++) into.fields.push(arena, placed[i]);
}

/*
 * The layout of a record whose constructors carry nothing - `RecordLayout::Enum`.
 *
 * Its value *is* its discriminant, so it is one integer and the numbers it can hold are exactly its
 * constructors' - which is a niche without anything having been refined. `Bool` is the smallest
 * case: two constructors in one byte, leaving two hundred and fifty-four patterns free, and that one
 * fact is what makes `Maybe(Bool)` a byte.
 */
static void layoutEnum(Layout layout, mir::RecordType& record, Repr& into) {
    auto range = mir::tagRangeOf(layout.base, record);
    auto bytes = storageBytesFor(range.bits);

    into.size = bytes;
    into.align = bytes;
    into.stride = bytes;
    into.discriminant = Discriminant::Word;
    into.discriminantBytes = bytes;

    // The bits the *numbers* need, which is not `range.bits`: that one is already rounded up to a
    // storage width, and what a container may co-pack is what the value actually spans.
    auto used = bitsUpTo(U64(range.highest > 0 ? range.highest : 0));
    into.scalarBits = range.isSigned ? range.bits : used;

    // Its value *is* its discriminant, so the tag's bit range is the record's own - stated rather
    // than left at zero, because what composes a shift onto a tag reads these two on every kind.
    into.discriminantBits = into.scalarBits;

    // A pinned negative takes the whole of its storage, since a sign bit is not a pattern anything
    // narrower holds - and a range that reaches below zero has no low free patterns to hand out.
    if(range.isSigned || range.highest < 0) return;

    into.niche = lowRangeNiche(bytes, used, U64(range.highest));
}

// The definition is in the header - see `hasPaddedWord`, which is where the argument for zeroing is.
bool hasPaddedWord(Layout layout, DeclPtr<mir::Type> type, U32 depth) {
    if(depth > 8) return false;

    auto base = layout.base;

    // An array's slots are its element repeated, so a niche in one is a niche in every slot.
    if(base[type]->kind == mir::Type::Array) {
        return hasPaddedWord(layout, ((mir::ArrayType*)base[type])->element, depth + 1);
    }

    if(base[type]->kind != mir::Type::Record) return false;

    auto repr = reprOf(layout, type);
    if(repr.opaque) return false;

    /*
     * The **box provenance word** is exactly what this question is about, one axis over: its bits
     * are set one at a time by the construction of each box, which is a read-modify-write, so a
     * record with fewer boxes than the word has bits has bits nothing writes. Zeroing the storage is
     * what makes the first of those `or`s read a zero rather than whatever the frame held.
     *
     * Asked ahead of the fields for the same reason the tag below is: the word is not a field, so
     * the walk over them would never see it.
     */
    if(repr.boxFlagsBytes) return true;

    /*
     * A bit-tagged sum's word is filled by read-modify-writes throughout - the arms below the tag
     * and the tag above them - so whatever is left over at the top is written by nothing. Asked
     * ahead of the fields because the tag is not one of them: an arm that carries nothing spends no
     * bits at all, and the field walk below would see a word only the tag is in.
     */
    if(repr.isBitTagged() &&
       repr.discriminantBitOffset + repr.discriminantBits < repr.size * 8) {
        return true;
    }

    for(auto field: repr.fields.contents(base)) {
        if(field.isPacked()) {
            if(((mir::RecordType*)base[type])->pin == mir::TypeLayout::Packed) {
                // A `@layout(packed)` declaration has already proved that every byte in its bit
                // stream is assigned by a field. The access word may be wider than one field, or
                // may be split byte-wise, so its unused access bits are not representation padding
                // and must not make us zero an otherwise complete record.
                continue;
            }

            // How far up the word anything reaches, over every field sharing it. A word whose run
            // stops short has spare bits, and they are exactly the ones nothing writes.
            U32 used = 0;
            for(auto other: repr.fields.contents(base)) {
                if(other.isPacked() && other.offset == field.offset) {
                    used = max(used, U32(other.bitOffset) + other.bitWidth);
                }
            }

            if(used < U32(field.wordBytes) * 8) return true;
            continue;
        }

        // A field holding a reference is one address with nothing spare in it, and what is on the
        // other end is storage of its own whose own allocation answers this question. Stopping here
        // is also what keeps the walk finite over a type that reaches itself.
        if(isReferenceValue(layout, field.occurrence)) continue;
        if(field.occurrence.type == type) continue;

        if(hasPaddedWord(layout, field.occurrence.type, depth + 1)) return true;
    }

    return false;
}

// The argument is in repr.h, beside the declaration.
bool storesBoxProvenance(Layout layout) {
    if(layout.program->roles.function(intrinsic::Known::isHeapAddress)) return false;
    return layout.program->asksBoxProvenance;
}

Repr reprOf(Layout layout, DeclPtr<mir::Type> type, DeclPtr<mir::OwnershipSchema> shape) {
    auto base = layout.base;
    auto& logical = *base[type];

    switch(logical.kind) {
        case mir::Type::Unit:
            return Repr {};

        case mir::Type::Int: {
            auto& integer = (mir::IntType&)logical;
            auto bits = intBitsOn(layout.target, integer);
            auto bytes = storageBytesFor(bits);

            Repr repr;
            repr.size = bytes;
            repr.align = bytes;
            repr.stride = bytes;
            repr.scalarBits = bits;

            /*
             * A **narrow** integer donates the patterns above it - `@bits(53) U64` cannot reach
             * anything from 2^53 upwards, and `WideInt` is the same shape without an attribute
             * having been written.
             *
             * The **niche** is unsigned only, and `scalarBits` deliberately is not. A signed narrow
             * value's patterns are not a contiguous low range - it reaches downwards as well, and
             * the free patterns are the gap in the middle, which is not a shape this description
             * has - but it is still a value that owns `bits` of its storage and may share the rest
             * with a neighbour. What that costs is one instruction: a packed read of a signed field
             * shifts up and back down arithmetically where an unsigned one shifts down and masks.
             */
            if(!integer.isSigned) repr.niche = lowRangeNiche(bytes, bits, lowMask(bits));

            return repr;
        }

        case mir::Type::Float: {
            auto bits = ((mir::FloatType&)logical).bits;
            Repr repr;
            repr.size = U32(bits / 8);
            repr.align = repr.size;
            repr.stride = repr.size;
            return repr;
        }

        /*
         * A **raw** pointer - one address, and deliberately no niche: `null()` and `isNull` are
         * ordinary Native intrinsics, so zero is a pattern a `%T` legally holds. A *borrow* is the
         * one that is non-null, and it reaches this table as a contract rather than as a kind - see
         * `membersOf`, which is where the two are told apart.
         */
        case mir::Type::Pointer: {
            Repr repr;
            repr.size = layout.target.pointerSize;
            repr.align = layout.target.pointerAlign;
            repr.stride = repr.size;
            return repr;
        }

        // A raw function pointer is one code word; an unconventioned function value is `{code, env}`.
        case mir::Type::Fun: {
            if(((mir::FunType&)logical).convention) {
                Repr repr;
                repr.size = layout.target.pointerSize;
                repr.align = layout.target.pointerAlign;
                repr.stride = repr.size;
                return repr;
            }

            Repr repr;
            repr.size = layout.target.funSize();
            repr.align = layout.target.pointerAlign;
            repr.stride = repr.size;
            return repr;
        }

        case mir::Type::Iter: {
            Repr repr;
            repr.size = layout.target.funSize();
            repr.align = layout.target.pointerAlign;
            repr.stride = repr.size;
            return repr;
        }

        /*
         * A fixed array - `n` elements at the element's **stride**.
         *
         * The stride and not the size, and that is the whole of this row: §10.3 gives a record's
         * trailing padding to whoever contains it, and two elements of an array are exactly the case
         * where nobody may land in it. `[Pad *3]` where `Pad` is nine bytes is forty-eight.
         *
         * No niche is republished. The first element's would be a correct one to lend, but a
         * container's elements are exactly the bytes the program assigns to, so there are no spare
         * patterns in them - `Maybe([T *4])` folding into a pattern `xs[0] = v` can write at will is
         * the failure that would be.
         */
        case mir::Type::Array: {
            auto& array = (mir::ArrayType&)logical;
            auto element = reprOf(layout, array.element);

            if(!array.hasFixedLength() || element.opaque) return opaqueRepr();

            Repr repr;
            repr.align = element.align ? element.align : 1;
            repr.size = U32(element.stride * array.length);
            repr.stride = repr.size;
            return repr;
        }

        /*
         * A vector - the lane width times the count, aligned to the whole.
         *
         * A vector move is one instruction against a naturally aligned address on every target that
         * has one, and a field holding one has to be placed where that instruction may reach it.
         */
        case mir::Type::Vector: {
            auto& vector = (mir::VectorType&)logical;
            auto lane = mir::laneBytes(base, layout.target.integers, vector.lane);

            if(!vector.hasFixedCount() || !vector.lanes || !lane) {
                return opaqueRepr();
            }

            Repr repr;
            repr.size = U32(lane * vector.lanes);
            repr.align = repr.size;
            repr.stride = repr.size;
            return repr;
        }

        /*
         * An atomic is its content's bytes at its content's width, and donates **no** niche and no
         * scalar form.
         *
         * No niche because the type exists to make one location indivisible: a container folding a
         * tag into an atomic's spare patterns would be writing a second thing into storage whose
         * whole contract is that one operation owns it. No scalar form for the same reason read
         * from the other end - a neighbour sharing its word would be read and written by the same
         * locked access, which is Analysis-Atomics.md §3.1's "regardless of how an ordinary `Bool`
         * is packed into another record".
         */
        case mir::Type::Atomic: {
            auto content = reprOf(layout, ((mir::AtomicType&)logical).content);
            if(content.opaque) return opaqueRepr();

            Repr repr;
            repr.size = content.size;
            repr.align = content.size;
            repr.stride = content.size;
            return repr;
        }

        case mir::Type::Record: {
            auto& record = (mir::RecordType&)logical;

            /*
             * The memo, on the record - and the one thing on a type that *is* a cache.
             *
             * A **shaped** occurrence is not the type's answer: `Maybe('v)` and `Maybe(v)` are one
             * type laid out two ways, the first holding a reference where the second holds the
             * payload, so a qualified occurrence is measured each time. There is one such occurrence
             * per signature that wrote one.
             */
            if(!shape && record.nativeRepr) return *base[DeclPtr<Repr>(record.nativeRepr)];

            Repr repr;
            if(record.layout == mir::RecordLayout::Enum) layoutEnum(layout, record, repr);
            else layoutRecord(layout, record, repr, shape);

            if(!shape) {
                auto stored = new (layout.program->decl) Repr(repr);
                record.nativeRepr = U32(DeclPtr<Repr>(stored - base).offset);
            }

            return repr;
        }

        /*
         * A type parameter, and a number written where a type is.
         *
         * The first has no layout in a body compiled once - what it costs is in the descriptor its
         * caller supplied, which is what `opaque` says. The second has no value at all: `Buf(4)`
         * manifests over its fields and `4` is not one of them.
         */
        case mir::Type::Variable:
            return opaqueRepr();

        case mir::Type::Const:
        case mir::Type::KindCount:
            break;
    }

    return Repr {};
}

} // namespace ssa
