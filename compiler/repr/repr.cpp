#include "repr.h"

/*
 * The layout search itself.
 *
 * One implementation, parameterized by ReprTarget, run bottom-up over the type graph: a type's
 * layout needs its members' layouts, so `of` recurses and memoizes rather than being scheduled as a
 * pass over a topological order. That is the same shape the old resolve-stage computation had, and
 * it terminates for the same reason - resolve has already rejected a type whose inline containment
 * is cyclic, so the guard here exists for a program that had errors rather than for a program that
 * is merely recursive.
 */

static U32 alignTo(U32 value, U32 alignment) {
    return alignment ? (value + alignment - 1) & ~(alignment - 1) : value;
}

// A newtype and a niche-folded sum both *are* their content, field placement included, so both copy
// the content's list rather than referring to it - a Repr owns what it hands out.
static void copyFields(const Array<FieldRepr>& from, Array<FieldRepr>& to) {
    to.reserve(U32(from.size()));
    for(auto& field: from) to.push(field);
}

U64 Niche::freeAbove() const {
    if(!exists()) return 0;

    // The number of patterns above `validEnd` in a `bytes`-wide word. A full 64-bit word has 2^64
    // of them when nothing is valid, which no count can hold - so this saturates, and every caller
    // only ever compares it against a constructor count.
    if(bytes >= 8) {
        return validEnd == maxLimit<U64> ? 0 : maxLimit<U64> - validEnd;
    }

    auto total = U64(1) << (U64(bytes) * 8);
    return validEnd >= total - 1 ? 0 : total - 1 - validEnd;
}

U64 NicheEncoding::patternOf(U16 constructor) const {
    // The payload constructor has no pattern of its own - it is recognized by the scrutinee being
    // inside the valid range - so the others are numbered around it.
    auto index = U64(constructor > payloadConstructor ? constructor - 1 : constructor);
    return ascending ? firstPattern + index : firstPattern - index;
}

ReprTarget nativeReprTarget() {
    ReprTarget target;
    target.pointerSize = 8;
    target.pointerAlign = 8;
    target.integerBits = 64;
    target.maxPackBits = 64;
    target.nullableRawPointers = true;
    target.packFields = true;
    target.bitTagSums = true;
    target.scalarizeRecords = true;
    target.foldNiches = true;
    return target;
}

/*
 * The JS family.
 *
 * The sizes are not byte counts of anything the host has - a JS value is not laid out - but they are
 * still the right numbers to compute with, because what they feed is the *packing* decision:
 * Design.md's "JS target packing" is entirely the observation that a host `number` holds 53
 * consecutive integer bits, so a record whose fields fit in 53 bits is one number. Setting
 * `integerBits` to 53 is that whole section.
 *
 * A reference-shaped value is given a size so that a record containing one still has a well-defined
 * field order; the JS backend reads the field *list*, never the offsets.
 */
ReprTarget jsReprTarget() {
    ReprTarget target;
    target.pointerSize = 8;
    target.pointerAlign = 8;
    target.integerBits = 53;

    /*
     * The same 53, but it was 32 first and the two are different questions.
     *
     * `integerBits` is what a *value* of this target can hold. This is what a *load* of one can be
     * taken apart with, and every operation that makes packing cheap here - `&`, `|`, `<<`, `>>>` -
     * is 32-bit in JS. Above 32 bits extracting a field becomes a divide and a mask and putting one
     * back becomes a subtract and a multiply, which is a different lowering rather than a free
     * extension of the same one, so it was left at 32 until it had been measured.
     *
     * It has been. On `{a: @bits(20), b: @bits(20)}` - fifty-two bits, and therefore one object
     * under the old bound - one `number` is seven times less retained memory and 2.85x the
     * construction rate against 0.84x on a field read, which is a bigger win than the scalar-integer
     * change that made the arithmetic available. benchmark/bits53-js/record-benchmark.mjs is that
     * measurement and codegen/js/place.cpp is the lowering; test/resolve/WidePack.yana is what says
     * the two forms compute the same numbers.
     *
     * What the raise needs from *this* file is `packableHere`: the bits fitting is not enough, since
     * a field also has to be a value this target can move in and out of a word, and an integer whose
     * canonical width is 64 is a `bigint` here rather than a `number`.
     */
    target.maxPackBits = 53;
    target.nullableRawPointers = true;

    /*
     * On.
     *
     * What this turns on is not bytes but the removal of the *object*: a record whose whole Repr fits
     * 32 bits and holds no reference becomes a `number`, with no allocation, no hidden class, nothing
     * to trace, a copy that is a register move, `===` for equality, and value semantics as a `Map`
     * key. Measured against the same record as an object, 95% less memory and 15x construction.
     *
     * What it cost was `Bool`. Scalarizing makes fields bit ranges, so a `&` of one carries a shift
     * and reads and writes through a mask, uniformly - one compiled `flip(&Bool)` serves a bit of a
     * scalarized record, a co-packed field and a whole local. That read-modify-write is the identity
     * on 0 or 1 and turns `true` into `1`, so `Bool` is now a number on this target; see `isBool` in
     * codegen/js/type.cpp, which measured the change as an improvement in its own right. The one
     * representation that keeps the host's form is `@layout(js)`, and a narrow field of one may not
     * be borrowed - there is nowhere to put the conversion. See placeIsHostPinnedField.
     */
    target.scalarizeRecords = true;

    /*
     * On, and separately from the flag above because on this target they are different features.
     *
     * This is the case scalarization cannot reach: a record that holds a reference, or something
     * else full-width, keeps its object and its narrow fields share one property rather than taking
     * one each. Measured on exactly that shape - five narrow fields and one reference - a co-packed
     * value is 44 bytes against 76, and the access cost is 9% on a read and 14% on a write because
     * the two ALU ops ride on a load that was happening anyway. Once the working set leaves cache the
     * sign flips outright and every access is 1.6x, since 1M records is 44 MB rather than 76.
     *
     * The original argument against it was that saving bytes "on a target with no cache line to save"
     * bought nothing, which is false - JS runs on the same hardware - and it was carrying the
     * conclusion. See Implementation-JS-Repr.md part 3.
     */
    target.packFields = true;

    /*
     * On, together, because on this target they are one feature.
     *
     * `foldNiches` was off here until `absentNiche` existed, and not because the lowering was missing
     * but because the *niche* was the wrong one: folding would have found a borrow's unreachable
     * address zero or the spare patterns of a packed word of an object, and neither is something a
     * host value can be tested for. With `Absent` in the search, what a folded `Maybe` becomes is what
     * Design.md asks for - `Maybe(Id)` is `number | null` and `Maybe(Person)` is `Person | null`, so a
     * host API can hand one straight back and a JS reader sees the shape they would have written.
     */
    target.absentNiche = true;
    target.foldNiches = true;
    return target;
}

ReprTable::~ReprTable() {
    for(auto entry: reprs) delete entry;
}

const Repr& ReprTable::of(TypePtr type, ReprRequirements) {
    static const Repr empty;
    if(!type) return empty;

    if(auto found = cache.getValue(U32(type))) return *found.unwrap();

    // A type currently being laid out is one resolve should already have rejected. Answering with an
    // empty Repr lets emission finish instead of recursing forever, and does not add a second
    // diagnostic about a declaration the user has already been told about.
    for(auto pending: inProgress) {
        if(pending == U32(type)) return empty;
    }

    inProgress.push(U32(type));

    auto owned = new Repr;
    compute(type, *owned);

    inProgress.pop();

    owned->stride = owned->stride ? owned->stride : alignTo(owned->size, owned->align);
    checkAbiContract(type, *owned);

    reprs.push(owned);
    cache.add(U32(type), owned);
    return *owned;
}

void ReprTable::checkAbiContract(TypePtr type, const Repr& repr) const {
    // A generic type has no layout here at all, so there is nothing to hold it to.
    if(repr.opaque) return;

    // A copy in a register is a copy of one register's worth. Anything resolve calls direct whose
    // representation outgrew that is a type admitted to `isDirectType` by its kind while its layout
    // says otherwise - and the failure would be silent, since the value would simply be truncated at
    // every load of it.
    assertTrue(!isDirectType(global, type) || repr.size <= target.pointerSize);
}

const FieldRepr* ReprTable::fieldOf(TypePtr type, U16 index) {
    auto& repr = of(type);
    return index < repr.fields.size() ? &repr.fields[index] : nullptr;
}

bool ReprTable::hasPaddedWord(TypePtr type, U32 depth) {
    auto& repr = of(type);
    if(repr.opaque || depth > 8) return false;

    /*
     * A bit tag's leftover patterns, which are exactly the packed case below wearing another name: the
     * tag is written by a read-modify-write, so the bits above it are whatever the storage held, and
     * the niche the record publishes is made of them.
     */
    if(repr.isBitTagged() &&
       repr.discriminantBitOffset + repr.discriminantBits < repr.size * 8) {
        return true;
    }

    /*
     * A *packed* word only, which is the whole of the difference.
     *
     * A narrow value that owns its storage is written by a store of the whole width - a `@bits(13)`
     * field is a two-byte store of a masked value - so its high bits are written every time and its
     * niche is true without anyone arranging it. A packed word is written by a read-modify-write that
     * preserves everything outside the field, so the bits above the run are whatever was there.
     */
    for(auto& field: repr.fields) {
        if(field.isPacked()) {
            // The packed fields of one word, gathered by asking how far up the word anything reaches.
            U32 used = 0;
            for(auto& other: repr.fields) {
                if(other.isPacked() && other.offset == field.offset) {
                    used = max(used, U32(other.bitOffset) + other.bitWidth);
                }
            }

            if(used < U32(field.wordBytes) * 8) return true;
        } else if(field.type && field.type != type && hasPaddedWord(field.type, depth + 1)) {
            return true;
        }
    }

    /*
     * A sum's payloads, which its own field list does not mention: a record with several constructors
     * describes their contents one Downcast at a time rather than as fields of itself, so the walk
     * above would stop at a tag and never look at what the tag selects between.
     */
    auto value = global[type];
    if(value->kind == Type::Record) {
        for(auto constructor: ((RecordType*)value)->constructors.contents(global)) {
            auto content = constructor.content;
            if(content && content != type && hasPaddedWord(content, depth + 1)) return true;
        }
    }

    return false;
}

void ReprTable::compute(TypePtr type, Repr& into) {
    auto value = global[type];

    // A type with a variable inside it has no layout here at all. A generic body reads what it needs
    // out of the environment its caller passed, which is what TypeDesc exists to carry.
    if(value->generic) {
        into.opaque = true;
        return;
    }

    switch(value->kind) {
        case Type::Int: {
            auto integer = (IntType*)value;

            // `bits` is how wide the value is in storage and `width` is the primitive it occupies
            // once loaded. A standalone value of a narrowed type still occupies its natural width -
            // packing is something a *container* does to a field - so the size here is the width's,
            // and the narrowing shows up as the niche the leftover patterns expose.
            auto storage = naturalBytes(integer->bits);
            into.size = storage;
            into.align = storage;
            into.scalarBits = integer->bits;
            into.niche = intNiche(*integer, 0);
            break;
        }
        case Type::Float: {
            auto width = ((FloatType*)value)->width == FloatType::Double ? 8u : 4u;
            into.size = width;
            into.align = width;
            break;
        }
        case Type::Ptr:
            into.size = target.pointerSize;
            into.align = target.pointerAlign;
            if(!target.nullableRawPointers) into.niche = addressNiche(0);
            break;
        case Type::Borrow:
            // A borrow is rung 2 of the reference ladder and always names live storage, so pattern
            // zero is genuinely unreachable. This is the niche `Maybe(&T)` folds into, and it is
            // declared once here for every sum type that will ever be built over a borrow.
            into.size = target.pointerSize;
            into.align = target.pointerAlign;
            into.niche = addressNiche(0);
            break;
        case Type::Fun:
            // `{code, env}` - see FunValueLayout, whose offsets this has to agree with.
            into.size = 2 * target.pointerSize;
            into.align = target.pointerAlign;
            break;
        case Type::Tup:
            computeTuple(*(TupType*)value, into);
            break;
        case Type::Record:
            computeRecord(*(RecordType*)value, into);
            break;
        default:
            // Unit, Error, and the kinds that are reserved but not constructible yet - Ref,
            // RegionPtr, Region, Array, Map. Reaching one of those with a value in hand is a
            // compiler bug rather than a layout question, and zero is what the resolver's own
            // computation answered for them too.
            break;
    }

    if(target.absentNiche) hostNiche(type, into);
}

/*
 * The niche a host value has, on a target where a value is not a word.
 *
 * Run over every representation after the search has produced whatever it produces, because the
 * question is about the *value* rather than about how the layout was arrived at: a tuple that got a
 * niche out of a packed word inside it and a borrow that got one out of an unreachable address both
 * end up here, and neither of those patterns is a thing a host value can be compared against.
 *
 * So the rule is one line. A representation that really is a number keeps what the search found,
 * because `v <= 15` is a comparison of a number and nothing else changes. Everything else takes
 * `null`, which is the only pattern it has - and which a value that is *already* nullable does not
 * have, since something is using it. That is `Maybe(Maybe(T))`, and it is the case this declines.
 */
void ReprTable::hostNiche(TypePtr type, Repr& into) {
    auto value = global[type];

    // Whether the host value really is a number or a bigint, which is the whole question: only for
    // one of those does a range of integers describe anything the value can be compared against.
    auto isNumber = false;

    switch(value->kind) {
        case Type::Int:
            isNumber = true;
            break;

        case Type::Float:
        case Type::Fun:
        case Type::Borrow:
            // A `number` with no spare patterns to speak of, a host function, and a reference. None
            // of them has a range to be outside of, and each of them has `null`.
            break;

        case Type::Tup:
            // One number where the Repr made it one, and an object otherwise. An object's spare bits
            // are inside a property, and a property of it is not something the object itself can be
            // compared against - so the niche a packed word republished is not the object's to give.
            isNumber = into.scalarBits != 0;
            break;

        case Type::Record: {
            auto record = (RecordType*)value;

            // A newtype is its content, this answer included, and has already copied it.
            if(record->layout == RecordType::Single) return;

            // An enum is its discriminant, which is a number.
            if(record->layout == RecordType::Enum) {
                isNumber = true;
                break;
            }

            /*
             * A folded record is its payload with one pattern taken out of it, so it is a number
             * exactly when the payload was - and the fold has already recorded which. A `Pattern` came
             * out of a number, an `Absent` out of a host value.
             */
            if(into.isNicheFolded()) {
                isNumber = !into.encoding.niche.isAbsent();
                break;
            }

            break;
        }

        default:
            // A raw pointer, whose absent value is spoken for - see `absentNiche`. Unit, Error and the
            // kinds that are reserved but not constructible have no value to be absent in the first
            // place, and answering `null` for one would hand out a niche over nothing.
            into.niche = Niche {};
            return;
    }

    /*
     * A number keeps what the search found wherever that is good for anything at all, and `fits(1)` is
     * the threshold because a pattern niche is at least as capable as an absent one everywhere else:
     * `Maybe(Rank)` folding into pattern 3 stays a number a containing record can co-pack, where
     * `Rank | null` would not be. A width with nothing spare - a full `Int`, a full `I64` - has only
     * `null`, and that is what makes `Maybe(Int)` the `number | null` a JS reader would have written.
     */
    if(isNumber && into.niche.fits(1)) return;

    // Spent. A folded record that took the absent pattern has no second one to give, which is what
    // makes `Maybe(Maybe(T))` decline while `Maybe(Maybe(Rank))` folds twice.
    if(!isNumber && into.isNicheFolded()) {
        into.niche = Niche {};
        return;
    }

    Niche niche;
    niche.kind = NicheKind::Absent;

    // Non-zero so that `exists` is true. There is no word and no width; whoever reads this compares
    // against `null` and never asks how many bytes that took.
    niche.bytes = 1;
    into.niche = niche;
}

// The natural storage of an integer of `bits` logical width: the smallest power-of-two byte count
// that holds it, which is what the machine has a load for. The rule itself lives in resolve, because
// the packing-candidate predicate there is stated in terms of it and the two have to agree.
U32 ReprTable::naturalBytes(U32 bits) const {
    return naturalStorageBits(bits) / 8;
}

/*
 * What an integer whose value range is narrower than its storage leaves over.
 *
 * This is the `@bits(n)` niche and the bounded-enum niche in one function, because they are one
 * fact: a word that can only hold `[0, 2^n)` cannot hold anything above it, and what is above it is
 * free for a discriminant. A type whose bits exactly fill its storage exposes nothing.
 */
Niche ReprTable::intNiche(const IntType& integer, U32 offset) const {
    auto bytes = naturalBytes(integer.bits);
    if(integer.bits >= bytes * 8) return {};

    // A signed narrow value occupies both ends of the range once sign-extended, so the patterns it
    // cannot produce are not one contiguous run. Declining is the honest answer rather than folding
    // into a range the value can reach.
    if(integer.isSigned) return {};

    Niche niche;
    niche.offset = offset;
    niche.bytes = U8(bytes);
    niche.validStart = 0;
    niche.validEnd = integer.bits >= 64 ? maxLimit<U64> : (U64(1) << integer.bits) - 1;
    return niche;
}

// The niche a value that is never zero exposes: pattern 0, and nothing else.
Niche ReprTable::addressNiche(U32 offset) const {
    Niche niche;
    niche.offset = offset;
    niche.bytes = U8(target.pointerSize);
    niche.validStart = 1;
    niche.validEnd = maxLimit<U64>;
    return niche;
}

void ReprTable::computeTuple(TupType& tuple, Repr& into) {
    U32 size = 0;
    U32 alignment = 1;
    auto count = tuple.fields.size();

    // Placed by index rather than pushed, because fields are not placed in the order they were
    // written and a packed word places several of them at once.
    into.fields.reserve(U32(count));
    for(Size i = 0; i < count; i++) into.fields.push(FieldRepr {});

    // The whole aggregate as one scalar, where it has that form and this target can hold it.
    if(target.scalarizeRecords && scalarizeTuple(tuple, into)) return;

    Array<U16> order;
    placementOrder(tuple, order);
    auto walk = toBuffer(order);

    Size at = 0;
    while(at < walk.length) {
        if(target.packFields) {
            auto next = packWord(tuple, into, walk, at, size, alignment);
            if(next != at) { at = next; continue; }
        }

        auto index = walk[at];
        auto field = tuple.fields.get(global, index);
        auto& member = of(field.type);
        auto memberSize = member.size;
        auto memberAlign = member.align;

        /*
         * A bit-field that shares its unit with nobody still occupies the whole unit under a pinned
         * layout, because C says so: `int d: 4` between two other members takes four bytes, not the
         * one its four bits would fit in. Only the *slot* widens - reading and writing it is still a
         * load of the value's own width at its own address, which on this byte order is the low end of
         * the unit and is what the narrow-reference ABI already assumes.
         */
        if(tuple.layout == TypeLayout::C) {
            if(auto unit = declaredUnitBits(global, field.type) / 8) {
                memberSize = max(memberSize, unit);
                memberAlign = max(memberAlign, unit);
            }
        }

        FieldRepr placed;
        placed.type = field.type;
        placed.wordBytes = U8(memberSize > 255 ? 0 : memberSize);

        size = alignTo(size, memberAlign);
        placed.offset = size;
        size += memberSize;
        alignment = max(alignment, memberAlign);

        // The first niche any field exposes becomes the aggregate's, republished at the offset the
        // field ended up at. First rather than largest, because a niche is only ever asked whether
        // it fits a constructor count and the search stops as soon as one does.
        if(!into.niche.exists() && member.niche.exists()) {
            into.niche = member.niche;
            into.niche.offset += placed.offset;
        }

        into.fields[index] = placed;
        at++;
    }

    into.size = alignTo(size, alignment);
    into.align = alignment;
}

/*
 * The whole aggregate as one integer - Implementation-JS-Repr.md part 1's shape, arrived at for
 * native first because it is where the bits are.
 *
 * A record whose every field is narrow *is* a run of co-packed fields and nothing else, so it costs
 * the natural storage of their total width: `{a: Bool, b: Bool}` is one byte holding two bits, and a
 * record containing that record co-packs those two bits into a word of its own rather than spending a
 * byte on them. Which records have this form is `scalarLayout`'s answer rather than this one, for the
 * usual reason - the span it reports is the mask width a callee holding a `&` of the whole aggregate
 * applies, so the two have to be the same number.
 *
 * Declining where the span is wider than this target can load is what `Repr::scalarBits` being a
 * target's answer is for: the record is then laid out as an ordinary aggregate below, and a `&` of it
 * is an ordinary address.
 *
 * A field that resolve did not name as a pack candidate is placed *unpacked* even here, which matters
 * for exactly one shape: a single-field record, whose one field is narrow but has nothing to share a
 * word with. Its scalar form and its plain layout are the same bytes - offset zero of a word its own
 * natural storage wide - so the only difference packing it would make is to take away the address
 * `addressOf` is still allowed to hand out. The contract runs one way, and this is the one place in
 * this function where that could have been broken silently.
 */
bool ReprTable::packableHere(TypePtr type, U32 depth) {
    if(!type || depth > 8) return false;

    auto value = global[type];
    switch(value->kind) {
        case Type::Int:
            return U32(((IntType*)global[canonicalType(global, type)])->bits) <= target.integerBits;
        case Type::Tup: {
            auto& tuple = *(TupType*)value;
            for(auto field: tuple.fields.contents(global)) {
                if(!packableHere(field.type, depth + 1)) return false;
            }

            return true;
        }
        case Type::Record: {
            auto& record = *(RecordType*)value;

            // An enum is its discriminant, which is a small integer on every target. A newtype is
            // its content. Anything else was never narrow to begin with - see valueWidth.
            if(record.layout == RecordType::Enum) return true;
            if(record.layout != RecordType::Single || record.constructors.isEmpty()) return false;

            return packableHere(record.constructors.get(global, 0).content, depth + 1);
        }
        default:
            return false;
    }
}

bool ReprTable::scalarizeTuple(TupType& tuple, Repr& into) {
    PackedRun run;
    Array<U32> offsets;
    if(!scalarLayout(global, tuple, run, &offsets)) return false;

    auto budget = min(target.maxPackBits, kMaxPackBits);
    if(run.span > budget) return false;

    // All of it or none of it: a scalar record *is* its fields' bits, so one field this target
    // cannot move in and out of a word is the whole record laid out the ordinary way.
    for(auto field: tuple.fields.contents(global)) {
        if(!packableHere(field.type)) return false;
    }

    auto bytes = naturalBytes(run.span);
    into.size = bytes;
    into.align = bytes;
    into.scalarBits = run.span;

    for(Size index = 0; index < offsets.size(); index++) {
        auto field = tuple.fields.get(global, index);

        FieldRepr placed;
        placed.type = field.type;
        placed.offset = 0;
        placed.wordBytes = U8(bytes);

        if(packCandidate(global, tuple, U16(index))) {
            placed.bitOffset = U8(offsets[index]);
            placed.bitWidth = U8(valueWidth(global, field.type).logical);
        }

        into.fields[index] = placed;
    }

    // Everything above the span, which is the niche `Maybe(Flags)` folds into - and the reason a
    // scalar record is worth having even where it saved no space.
    Niche niche;
    niche.offset = 0;
    niche.bytes = U8(bytes);
    niche.validStart = 0;
    niche.validEnd = (U64(1) << run.span) - 1;
    into.niche = niche;

    return true;
}

/*
 * The order fields are *placed* in, which is not the order they were written in.
 *
 * Two groups. Everything that owns its storage goes first, widest alignment first and then widest
 * size, which is the ordinary way to spend padding: a `U64` after a `Bool` costs seven bytes of hole,
 * and the same two fields the other way round cost none. Then the pack candidates, in `packOrder`,
 * because a run of bit-fields wants to end up in one word and a word placed last can be as narrow as
 * what is left over rather than as wide as the alignment it landed on.
 *
 * `@layout(c)` is declaration order, one group, and the reordering above is exactly what it opts out
 * of. A run of bit-fields is then whatever consecutive candidates the declaration wrote, which is
 * what makes `{a: @bits(4), b: U64, c: @bits(4)}` two units under a pin and one word without it.
 */
void ReprTable::placementOrder(TupType& tuple, Array<U16>& into) {
    auto count = tuple.fields.size();

    if(tuple.layout != TypeLayout::Auto) {
        for(U16 i = 0; i < count; i++) into.push(i);
        return;
    }

    Array<U16> packed;
    for(U16 i = 0; i < count; i++) {
        if(target.packFields && packCandidate(global, tuple, i)) packed.push(i);
        else into.push(i);
    }

    // Insertion sort, and stable, so that two fields a target has no reason to distinguish stay in
    // the order they were declared - the layout dump of a record is easier to read against its
    // declaration that way, and it is one fewer thing that changes when a field is added.
    for(Size i = 1; i < into.size(); i++) {
        auto index = into[i];
        auto& member = of(tuple.fields.get(global, index).type);
        auto at = i;

        while(at > 0) {
            auto& previous = of(tuple.fields.get(global, into[at - 1]).type);
            if(previous.align > member.align) break;
            if(previous.align == member.align && previous.size >= member.size) break;

            into[at] = into[at - 1];
            at--;
        }

        into[at] = index;
    }

    packOrder(global, tuple, packed);
    for(auto index: packed) into.push(index);
}

/*
 * One word of co-packed fields - Design.md's `Header`, and the half of Design.md's "Packed fields
 * and mutable borrowing" that this file is responsible for.
 *
 * Which fields may be co-packed is resolve's answer, not this one: `packCandidate` is stated over
 * the logical type so that it reads the same on every target, and this may pack fewer of them but
 * never more. What is decided here is how many of a run fit one word and how wide that word is.
 *
 * Returns the index to continue from, or `first` unchanged when nothing was packed - which the
 * caller takes as "place this field the ordinary way".
 */
Size ReprTable::packWord(TupType& tuple, Repr& into, Buffer<const U16> order, Size first, U32& size,
                         U32& alignment) {
    if(!packCandidate(global, tuple, order[first])) return first;

    /*
     * As many consecutive candidates of the placement order as fit the widest word this target can
     * load and mask in one go. The placement itself - and the straddle rule that decides how much of
     * the word a run actually spends - is `packBits` in resolve/type.cpp, because the span it reports
     * is also the mask width a callee holding a reference to a whole scalar aggregate applies.
     *
     * `min` with the language's budget rather than the target's alone, because a target wider than
     * `kMaxPackBits` would pack a run `valueWidth` had already told resolve was too wide to be one.
     */
    Array<U16> run;
    for(auto at = first; at < order.length && packCandidate(global, tuple, order[at]); at++) {
        // The run ends at a field this target cannot move in and out of a word, rather than the
        // whole record declining as it does above: the fields before it are still a word, and the
        // one that stopped it gets a property of its own like any unpacked field.
        if(!packableHere(tuple.fields.get(global, order[at]).type)) break;

        run.push(order[at]);
    }

    auto budget = min(target.maxPackBits, kMaxPackBits);
    Array<U32> offsets;
    auto placed = packBits(global, tuple, toBuffer(run), budget, &offsets);

    auto used = placed.span;
    auto last = first + placed.count;

    // A field alone in a word is not packed. It keeps its natural storage and therefore its
    // address, which costs nothing in space and saves every borrow of it a temporary - see tier 0
    // in Design.md. This is reached at the tail of a run too long for one word.
    if(placed.count < 2) return first;

    /*
     * How wide the word is. As narrow as the bits need, except under a pinned layout, where it is as
     * wide as the declared type of the widest bit-field in the run - two `int x: 4` share four bytes
     * in C rather than one, and the whole point of the pin is that they do here too.
     */
    auto wordBytes = naturalBytes(used);
    if(tuple.layout == TypeLayout::C) {
        U32 unit = 0;
        for(Size at = 0; at < placed.count; at++) {
            unit = max(unit, declaredUnitBits(global, tuple.fields.get(global, run[at]).type));
        }

        if(unit) wordBytes = alignTo(used, unit) / 8;
    }

    size = alignTo(size, wordBytes);

    auto offset = size;
    size += wordBytes;
    alignment = max(alignment, wordBytes);

    for(Size at = 0; at < placed.count; at++) {
        auto index = run[at];
        auto field = tuple.fields.get(global, index);

        FieldRepr entry;
        entry.type = field.type;
        entry.offset = offset;
        entry.wordBytes = U8(wordBytes);
        entry.bitOffset = U8(offsets[at]);
        entry.bitWidth = U8(valueWidth(global, field.type).logical);
        into.fields[index] = entry;
    }

    /*
     * What the word has left over, which is a better niche than any of these fields could have
     * donated alone: two booleans in one byte leave 252 patterns free where each of them separately
     * left the high half of its own word.
     *
     * A packed field never republishes its own niche - there is no whole word for it to be a niche
     * *of* - so this is the only way a packed run contributes one.
     */
    if(!into.niche.exists() && used < wordBytes * 8) {
        Niche niche;
        niche.offset = offset;
        niche.bytes = U8(wordBytes);
        niche.validStart = 0;
        niche.validEnd = (U64(1) << used) - 1;
        into.niche = niche;
    }

    return last;
}

void ReprTable::computeRecord(RecordType& record, Repr& into) {
    auto constructors = record.constructors.contents(global);

    if(record.layout == RecordType::Single) {
        auto content = constructors.size() ? constructors[0].content : nullptr;
        if(!content) return;

        // A newtype is its content, niche included - which is what makes a niche declared once on
        // one type benefit every wrapper over it without the wrapper knowing.
        auto& inner = of(content);
        into.size = inner.size;
        into.align = inner.align;
        into.scalarBits = inner.scalarBits;
        into.niche = inner.niche;
        copyFields(inner.fields, into.fields);
        into.payloadOffset = 0;
        return;
    }

    U32 payloadSize = 0;
    U32 payloadAlign = 1;

    for(auto constructor: constructors) {
        if(!constructor.content || isUnit(global, constructor.content)) continue;

        auto& content = of(constructor.content);
        payloadSize = max(payloadSize, content.size);
        payloadAlign = max(payloadAlign, content.align);
    }

    if(record.layout == RecordType::Enum) {
        // A payload-free sum *is* its discriminant, so it is a scalar of however many bits its
        // constructor count needs - which is what lets a pair of `Bool`s share a byte.
        into.scalarBits = valueWidth(global, (Type*)&record - global).logical;

        /*
         * No payload at all: the value is its discriminant, and it costs what that discriminant
         * costs. A `Bool` is one byte and a three-constructor enum is one byte, rather than the four
         * this used to spend unconditionally.
         *
         * That width is not a saving of its own - nothing holds a lone `Bool` more cheaply for it -
         * but of what it does to the records containing one. An enum field that is *not* co-packed
         * still occupies its own storage, so a four-byte `Bool` made `{a: U8, b: Bool}` eight bytes
         * and dragged the record's alignment to four; at one byte the same record is two, with both
         * fields addressable and read by an ordinary load. It is also what keeps the two answers
         * about a scalar record consistent: `naturalBytes(span)` is the size, and a field left
         * unpacked inside it has to fit that size or its store writes over the record.
         *
         * The unused patterns above the last constructor are the niche `Maybe(Bool)` and friends
         * fold into, and they are now the patterns of a byte rather than of a word.
         */
        auto bytes = naturalBytes(into.scalarBits);
        into.size = bytes;
        into.align = bytes;
        into.discriminant = DiscriminantKind::Word;
        into.discriminantBytes = bytes;

        // Zero rather than "after the discriminant", because there is no payload to be after it.
        // A Downcast into a payload-free constructor projects to nothing and must not move the
        // address it started from.
        into.payloadOffset = 0;

        Niche niche;
        niche.bytes = U8(bytes);
        niche.validStart = 0;
        niche.validEnd = constructors.size() ? constructors.size() - 1 : 0;
        into.niche = niche;
        return;
    }

    if(foldNiche(record, into)) return;
    if(scalarizeSum(record, into, payloadSize, payloadAlign)) return;

    into.discriminant = DiscriminantKind::Word;
    into.discriminantBytes = 4;
    into.payloadOffset = alignTo(4, payloadAlign);
    into.align = max(4u, payloadAlign);
    into.size = alignTo(into.payloadOffset + payloadSize, into.align);
}

/*
 * A tag of bits above the payload rather than a word in front of it.
 *
 * The fallback below spends four bytes on a tag and then aligns the payload behind it, so
 * `data Small = A(U8) | B(U8)` costs eight bytes to hold one byte and one bit. This is the case
 * niche folding cannot reach - `U8` fills its storage and has no impossible pattern - and it needs
 * none: the *tag* is what becomes small, not the payload.
 *
 * ## Why the payload keeps its own layout
 *
 * Each constructor's content is placed at offset zero exactly as it is laid out standalone, and the
 * tag goes strictly above the widest of them - above the whole `size` of it, padding included, and
 * not merely above the last bit any of its fields reaches. That costs a little density, and it is
 * what makes the rest of the compiler need no changes at all:
 *
 *  - a Downcast adds `payloadOffset`, which is zero, so the place walk reaches a payload field at
 *    exactly the offset `fieldOf(content, i)` reports. Nothing has to know that a payload is inside a
 *    tagged word, which is what keeps this out of resolve entirely;
 *  - a whole-payload *copy* moves `size` bytes, and the tag is past them. Were the tag placed in the
 *    padding a payload's alignment left over, `s = A(t)` for an aggregate `t` would memcpy over it -
 *    and whether it did would depend on whether the tag write happened to be emitted first.
 *
 * So the win is entirely "four bytes of tag become a few bits", which is the whole of the cost for
 * every payload of one, two or three bytes, and is why this only accepts a layout that is genuinely
 * smaller than the word-tagged one: a bit tag is a shift and a mask where a tag word is a load, so
 * where the two cost the same number of bytes the word is the better answer.
 *
 * Nothing about this makes the record a *scalar*: `scalarBits` stays zero, so a bit-tagged sum is
 * never co-packed into a parent and a `&` of one is an ordinary address. That matches `valueWidth` in
 * resolve, which declines a payload-carrying sum - and it is deliberately narrower than what the bits
 * would allow, because a sum that could be co-packed would also have to be borrowable as a bit range,
 * and the tag would then be at a shift the callee could not know.
 */
bool ReprTable::scalarizeSum(RecordType& record, Repr& into, U32 payloadSize, U32 payloadAlign) {
    if(!target.bitTagSums) return false;

    auto constructors = record.constructors.size();
    if(!constructors || !payloadSize) return false;

    U32 tagBits = 1;
    while((Size(1) << tagBits) < constructors) tagBits++;

    // The payload's whole storage, so that a copy of one cannot reach the tag. Everything above it is
    // the tag and then the patterns the tag does not use.
    auto payloadBits = payloadSize * 8;
    auto span = payloadBits + tagBits;

    auto budget = min(target.maxPackBits, kMaxPackBits);
    if(span > budget) return false;

    auto bytes = naturalBytes(span);
    auto alignment = max(bytes, payloadAlign);

    // What the word-tagged form below would have cost. Equal is not good enough - see above.
    auto wordAlign = max(4u, payloadAlign);
    auto wordSize = alignTo(alignTo(4, payloadAlign) + payloadSize, wordAlign);
    if(alignTo(bytes, alignment) >= wordSize) return false;

    into.size = alignTo(bytes, alignment);
    into.align = alignment;
    into.payloadOffset = 0;
    into.discriminant = DiscriminantKind::Bits;
    into.discriminantBytes = bytes;
    into.discriminantBitOffset = payloadBits;
    into.discriminantBits = tagBits;

    /*
     * Everything above the tag, which is what makes `Maybe` of a bit-tagged sum one word rather than
     * a second tag in front of it.
     *
     * Those patterns are only genuinely free if they are zero, and nothing writes them: a tag write
     * is a read-modify-write of the bits it owns and a payload write stays below them. So this niche
     * is true because the storage was zeroed when it was allocated, which is what hasPaddedWord
     * arranges - the same bargain a co-packed word's leftover bits are held to.
     */
    if(span < into.size * 8) {
        Niche niche;
        niche.offset = 0;
        niche.bytes = U8(min(into.size, 8u));
        niche.validStart = 0;
        niche.validEnd = (U64(1) << span) - 1;
        into.niche = niche;
    }

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
 */
bool ReprTable::foldNiche(RecordType& record, Repr& into) {
    if(!target.foldNiches) return false;

    auto constructors = record.constructors.contents(global);
    auto others = U64(constructors.size() - 1);

    for(auto constructor: constructors) {
        if(!constructor.content || isUnit(global, constructor.content)) continue;

        auto& content = of(constructor.content);
        if(!content.niche.fits(others)) continue;

        // Every other constructor has to fit *inside* this payload, since the folded record is
        // exactly this payload and there is nowhere else for their contents to go. In practice that
        // means they are payload-free, which is the `Nothing`/`Just(a)` shape this exists for.
        auto usable = true;
        for(auto other: constructors) {
            if(other.index == constructor.index) continue;
            if(other.content && !isUnit(global, other.content)) { usable = false; break; }
        }

        if(!usable) continue;

        into.size = content.size;
        into.align = content.align;
        copyFields(content.fields, into.fields);
        into.discriminant = DiscriminantKind::Niche;
        into.payloadOffset = 0;

        auto& encoding = into.encoding;
        encoding.niche = content.niche;
        encoding.payloadConstructor = U16(constructor.index);

        /*
         * One pattern, and no arithmetic over it: the other constructor *is* the host's absent value,
         * so there is nothing to number and nothing left over. `fits` allowed exactly one of them
         * through, so this is always the two-constructor shape.
         */
        if(content.niche.isAbsent()) {
            encoding.firstPattern = 0;
            encoding.ascending = true;
            into.niche = Niche {};
            return true;
        }

        // Prefer the patterns below the valid range, so that a `Maybe` over a non-null address gets
        // `Nothing == 0` and becomes a plain nullable pointer - the representation a C programmer
        // would have written, and the one a debugger can read.
        if(content.niche.freeBelow() >= others) {
            encoding.firstPattern = content.niche.validStart - 1;
            encoding.ascending = false;
        } else {
            encoding.firstPattern = content.niche.validEnd + 1;
            encoding.ascending = true;
        }

        // The folded record has consumed the patterns it needed; whatever is left over is still a
        // niche for whatever contains *this* record in turn.
        auto remaining = content.niche;
        if(encoding.ascending) {
            remaining.validEnd = encoding.firstPattern + others - 1;
        } else {
            remaining.validStart = encoding.firstPattern - (others - 1);
        }

        into.niche = remaining.freeBelow() || remaining.freeAbove() ? remaining : Niche {};
        return true;
    }

    return false;
}
