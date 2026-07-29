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
    target.nullableRawPointers = true;
    target.packFields = true;
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
    target.nullableRawPointers = true;
    target.packFields = true;
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
    reprs.push(owned);
    cache.add(U32(type), owned);
    return *owned;
}

const FieldRepr* ReprTable::fieldOf(TypePtr type, U16 index) {
    auto& repr = of(type);
    return index < repr.fields.size() ? &repr.fields[index] : nullptr;
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
}

// The natural storage of an integer of `bits` logical width: the smallest power-of-two byte count
// that holds it, which is what the machine has a load for.
U32 ReprTable::naturalBytes(U32 bits) const {
    if(bits <= 8) return 1;
    if(bits <= 16) return 2;
    if(bits <= 32) return 4;
    return 8;
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

    for(Size i = 0; i < tuple.fields.size(); i++) {
        auto field = tuple.fields.get(global, i);
        auto& member = of(field.type);

        FieldRepr placed;
        placed.type = field.type;
        placed.wordBytes = U8(member.size > 255 ? 0 : member.size);

        size = alignTo(size, member.align);
        placed.offset = size;
        size += member.size;
        alignment = max(alignment, member.align);

        // The first niche any field exposes becomes the aggregate's, republished at the offset the
        // field ended up at. First rather than largest, because a niche is only ever asked whether
        // it fits a constructor count and the search stops as soon as one does.
        if(!into.niche.exists() && member.niche.exists()) {
            into.niche = member.niche;
            into.niche.offset += placed.offset;
        }

        into.fields.push(placed);
    }

    into.size = alignTo(size, alignment);
    into.align = alignment;
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
        // No payload at all: the value is its discriminant. The unused patterns above the last
        // constructor are the niche `Maybe(Bool)` and friends fold into.
        into.size = 4;
        into.align = 4;
        into.discriminant = DiscriminantKind::Word;
        into.discriminantBytes = 4;

        // Zero rather than "after the discriminant", because there is no payload to be after it.
        // A Downcast into a payload-free constructor projects to nothing and must not move the
        // address it started from.
        into.payloadOffset = 0;

        Niche niche;
        niche.bytes = 4;
        niche.validStart = 0;
        niche.validEnd = constructors.size() ? constructors.size() - 1 : 0;
        into.niche = niche;
        return;
    }

    if(foldNiche(record, into)) return;

    into.discriminant = DiscriminantKind::Word;
    into.discriminantBytes = 4;
    into.payloadOffset = alignTo(4, payloadAlign);
    into.align = max(4u, payloadAlign);
    into.size = alignTo(into.payloadOffset + payloadSize, into.align);
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
