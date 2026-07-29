#pragma once

#include "../resolve/type.h"
#include "../resolve/inst.h"

/*
 * Physical representation - Design.md's "Representation and layout", Design-Memory §9, and the
 * plan in Implementation-Repr.md.
 *
 * Every type has a *logical* shape - its fields, its constructors, the operations over it - and a
 * *physical* representation. This is where the second one is decided, and the whole of it lives
 * behind this header: nothing in `compiler/resolve` knows how wide a `Point` is or which bit of a
 * word `Header.length` occupies.
 *
 * ## Why this is not in resolve
 *
 * Implementation-Repr.md left the question open ("whether Repr computation belongs entirely inside
 * compiler/resolve or partly in compiler/lower... this should be revisited once the resolver's
 * actual pass structure is working again"). It is answered here, in favour of the later stage, and
 * the reason is the JS target rather than tidiness.
 *
 * Repr is a family *per logical type and per target*, because the two targets' packing budgets are
 * different in kind and not only in size: native's niches are bit patterns an address or a narrowed
 * integer cannot hold, and JS's is `null`, which every non-null host value excludes. `Maybe(Id)` for
 * `alias Id = @bits(53) U64` is one machine word on native with the tag in the eleven bits a 53-bit
 * value leaves over, and `number | null` on JS. Same search, two answers. A Repr computed during
 * resolution would have to produce both at once and hand the pair to whichever backend ran, which is
 * exactly the coupling between the resolver and the code generators this arrangement removes.
 *
 * So resolve resolves, and a target computes layout when it is about to emit. What resolve keeps is
 * the half that is genuinely target-independent:
 *
 *  - the *logical* shape - fields, constructors, `RecordType::layout` - which is what this pass
 *    reads;
 *  - the inline-containment cycle check, so "this type needs an indirection" stays one diagnostic
 *    reported once against source rather than the same diagnostic from each backend;
 *  - `ReprRequirements` (see resolve/inst.h), which is what a *variant* is selected by. That
 *    analysis computes requirements, not layout, and it belongs with the ownership passes that
 *    produce it.
 *
 * ## What a target decides
 *
 * `ReprTarget` is every number and rule that differs between the two, and a `ReprTable` is one
 * target's answers for one program. A backend owns its table; two backends never share one, and
 * neither can see the other's. Adding a target is a `ReprTarget` rather than a pass.
 */

struct Module;

/*
 * A niche: the bit patterns one word of a representation can never legally hold.
 *
 * This is the whole of what makes discriminant folding a general algorithm rather than one
 * optimization per type pair, per Design.md: a niche is *declared once, by the type that has one*,
 * and every sum type built over that type benefits without anyone touching the sum type. `Maybe`,
 * `Result` and a library-defined wrapper all fold through the same search because none of them
 * knows what a niche is.
 *
 * The description is deliberately one contiguous valid range rather than a general pattern set.
 * Implementation-Repr.md asks for exactly that - "a first implementation should probably support
 * only a small number of niche shapes rather than a fully general constraint solver" - and the three
 * shapes that matter all fit it:
 *
 *  - a non-null address: `validStart == 1`, so pattern 0 is free;
 *  - a `@bits(n)` integer in a wider word: `validEnd == 2^n - 1`, so everything above is free;
 *  - a bounded enum discriminant: `validEnd == constructorCount - 1`, likewise.
 *
 * `bytes == 0` means no niche. Where a target's values are host objects rather than words, `bytes`
 * is zero and `host` carries the answer instead - see HostNiche.
 */
struct Niche {
    // Where the scrutinee word sits inside the representation that exposes this niche, and how wide
    // it is. A niche found in a nested field is republished by its parent with the offset adjusted,
    // which is what "recursively through nested fields" means in practice.
    U32 offset = 0;
    U8 bytes = 0;

    // The inclusive range of patterns a valid value can produce. Everything outside it is free.
    U64 validStart = 0;
    U64 validEnd = 0;

    bool exists() const { return bytes != 0; }

    // How many patterns are free below and above the valid range. Saturating, because a 64-bit word
    // whose valid range is empty has 2^64 free patterns and no count can hold that - and every
    // caller only ever compares the answer against a constructor count.
    U64 freeBelow() const { return validStart; }
    U64 freeAbove() const;

    // Whether this niche can distinguish `count` alternatives on top of the value itself.
    bool fits(U64 count) const { return exists() && (freeBelow() >= count || freeAbove() >= count); }
};

/*
 * Where one field of an aggregate physically sits.
 *
 * `bitWidth == 0` is the ordinary case: the field owns its storage, starting at `offset`, and is
 * read and written as a whole. A non-zero `bitWidth` is a co-packed field - `@bits(n)`, or a target
 * that packed two booleans into one byte - and then the access is a load of the `wordBytes`-wide
 * word at `offset`, a shift by `bitOffset` and a mask, rather than a load of the field itself.
 *
 * Two fields whose `offset`/`wordBytes` name overlapping storage alias each other for exclusivity
 * (Design-Memory §5.1, §12): otherwise two independent write-backs race to merge into one word and
 * whichever commits second silently clobbers the first, which is the classic C bitfield hazard.
 * `sharesStorageWith` is what the borrow checker asks.
 */
struct FieldRepr {
    TypePtr type = nullptr;
    U32 offset = 0;
    U8 wordBytes = 0;
    U8 bitOffset = 0;
    U8 bitWidth = 0;

    bool isPacked() const { return bitWidth != 0; }

    bool sharesStorageWith(const FieldRepr& other) const {
        auto end = offset + (wordBytes ? wordBytes : 0u);
        auto otherEnd = other.offset + (other.wordBytes ? other.wordBytes : 0u);
        return offset < otherEnd && other.offset < end;
    }
};

/*
 * How a sum type says which constructor it holds.
 *
 * `None` is a single-constructor record, which has nothing to say. `Word` is the explicit
 * discriminant this compiler has always emitted - a separate integer in front of the payload.
 * `Niche` is the folded form: the discriminant *is* a pattern of the payload's own storage that no
 * payload value can produce, so the record costs exactly its largest constructor.
 */
enum class DiscriminantKind : U8 {
    None,
    Word,
    Niche,
};

/*
 * The niche encoding of a sum type's discriminant.
 *
 * One constructor keeps the payload untouched and is recognized by the scrutinee word being *inside*
 * the valid range; every other constructor is one pattern outside it. `patternOf` is the encoding
 * and `constructorOf` the decoding, and they are here rather than at the two call sites so that the
 * backends cannot disagree about which pattern meant what.
 */
struct NicheEncoding {
    Niche niche;

    // The constructor whose payload occupies the storage. Every other constructor is a pattern.
    U16 payloadConstructor = 0;

    // The first pattern handed out, and whether they run upwards from it. Patterns are taken from
    // below the valid range when there is room there (so a non-null pointer's `Nothing` is 0, which
    // is what makes `Maybe(&T)` a plain nullable pointer) and from above it otherwise.
    U64 firstPattern = 0;
    bool ascending = true;

    U64 patternOf(U16 constructor) const;
};

/*
 * One type's physical representation for one target.
 *
 * Produced by ReprTable::of and immutable afterwards. A caller holds a reference for as long as it
 * needs one: the table never rewrites an entry, because a Repr computed while a dependent one was
 * being built would have to be invalidated and nothing is arranged to notice.
 */
struct Repr {
    U32 size = 0;
    U32 align = 1;

    // What indexing homogeneous storage advances by. `alignUp(size, align)` for everything today,
    // and explicit because a packed element or a target ABI may choose otherwise.
    U32 stride = 0;

    // The niche this representation exposes to whatever contains it, if any.
    Niche niche;

    // Aggregates: one entry per field of the tuple, in field order. Empty for a scalar.
    Array<FieldRepr> fields;

    // Sum types.
    DiscriminantKind discriminant = DiscriminantKind::None;
    U32 payloadOffset = 0;
    U32 discriminantBytes = 0;
    NicheEncoding encoding;

    // Set where the type has no layout on this target because it is not concrete. A generic body
    // reads what it needs out of the environment its caller passed instead.
    bool opaque = false;

    bool isNicheFolded() const { return discriminant == DiscriminantKind::Niche; }
};

/*
 * Everything one target decides differently, and nothing else.
 *
 * A `ReprTable` is this plus a cache, so the difference between the native and the JS families is
 * entirely the object handed to the table. That is what keeps the search - bottom-up over the type
 * graph, niche first, discriminant word as the fallback - one implementation with two answers rather
 * than two implementations that have to be kept saying the same thing.
 */
struct ReprTarget {
    // The width and alignment of an address. JS has no addresses, and answers with what its own
    // reference values cost so that a size computed over a pointer-shaped field is not zero.
    U32 pointerSize = 8;
    U32 pointerAlign = 8;

    // The widest integer a value can occupy without being split. 64 on native; 53 on JS, which is
    // the point at which a host `number` stops representing consecutive integers - see Design.md's
    // "JS target packing", where this single number is the whole of that section's budget.
    U32 integerBits = 64;

    // Whether an address is known never to be null, and therefore exposes pattern 0 as a niche.
    //
    // False for `%T`. A raw pointer is Design-Memory's rung 6 and `null()`/`isNull` are ordinary
    // Native intrinsics that the bump allocator in resolve/native.cpp is written in terms of, so
    // claiming its zero as a niche would quietly break the allocator. Borrows and the indirection
    // the compiler inserts for a recursive type are the ones that are genuinely non-null.
    bool nullableRawPointers = true;

    /*
     * Which end of a word this target puts the low byte at.
     *
     * A fact about the *reader* rather than about this compiler, which is the whole reason it is a
     * field: writing the host's bytes happens to be right for x64-on-x64 and is silently wrong for
     * any pair that disagrees. Everything that turns a value into bytes - a scalar global's
     * initializer, a witness table's words - goes through a writer at this order.
     */
    ByteOrder byteOrder = LittleEndian;

    /*
     * The two optimizations that change how a *value is accessed* rather than only how wide it is.
     *
     * Both are off, and they are off together for one reason: each makes an access stop being a
     * plain load at an offset. A co-packed field is a load of its containing word plus a shift and a
     * mask; a folded discriminant is not stored anywhere at all, and reading it is a comparison of
     * the payload's own bits against the range its type can reach. Neither is expressible as the
     * `place + offset` that both backends currently lower a field access to, so turning either on
     * without the encode/decode step in front of it does not produce a smaller value - it produces a
     * wrong one, silently, in every pattern match in the program.
     *
     * They are flags rather than absent code because the decision half is what the search above
     * already computes and is worth having reviewable on its own: `niche` is published for every
     * type here, so what remains is consuming it. Once the access lowering exists, turning these on
     * is the change, and being able to turn them back off is what distinguishes "the feature works"
     * from "the fixture agrees" when the layouts move.
     */
    bool packFields = false;
    bool foldNiches = false;
};

/*
 * One target's layout answers for one program.
 *
 * Keyed on the interned type, which is what makes the cache sound: two spellings of one type are one
 * `TypePtr`, and a Repr is a function of the type and the target alone.
 *
 * `ReprRequirements` is accepted and currently ignored beyond selecting the canonical variant. It is
 * in the signature rather than added later because it is what Implementation-Storage.md's Repr
 * ladder selects on, and a table that had to grow a key later would have every caller to update.
 */
struct ReprTable {
    ReprTable(GlobalBase global, const ReprTarget& target): global(global), target(target) {}
    ~ReprTable();

    ReprTable(const ReprTable&) = delete;
    ReprTable& operator = (const ReprTable&) = delete;

    const Repr& of(TypePtr type, ReprRequirements requirements = {});

    U32 sizeOf(TypePtr type) { return of(type).size; }
    U32 alignOf(TypePtr type) { return of(type).align; }
    U32 strideOf(TypePtr type) { return of(type).stride; }

    // The placement of one field of a tuple, or of the content tuple of a single-constructor record.
    // Null when the type has no such field, which is a compiler bug rather than a program error.
    const FieldRepr* fieldOf(TypePtr type, U16 index);

    GlobalBase global;
    ReprTarget target;

private:
    void compute(TypePtr type, Repr& into);
    void computeTuple(TupType& tuple, Repr& into);
    void computeRecord(RecordType& record, Repr& into);
    bool foldNiche(RecordType& record, Repr& into);

    U32 naturalBytes(U32 bits) const;
    Niche intNiche(const IntType& integer, U32 offset) const;
    Niche addressNiche(U32 offset) const;

    /*
     * The entries, owned separately from the index.
     *
     * A Repr is handed out by reference and computing one asks for its members', so an entry has to
     * keep its address while further entries are being added underneath it. Storing the values in
     * the map would move them on the next rehash and leave every reference taken so far pointing at
     * freed storage - which would be a use-after-free that only appears once a program has enough
     * types to grow the table.
     */
    HashMap<U32, Repr*> cache;
    Array<Repr*> reprs;

    // The types whose layout is currently being computed, so that a cycle answers rather than
    // recurses forever. Resolve has already reported a genuinely cyclic type; reaching one here
    // means the program had errors, and an empty Repr is the answer that lets emission finish
    // without a second diagnostic about the same declaration.
    Array<U32> inProgress;
};

// The two families, as the parameters that produce them. Free functions rather than constants so
// that a target can be varied by a setting later without every caller learning about it.
ReprTarget nativeReprTarget();
ReprTarget jsReprTarget();
