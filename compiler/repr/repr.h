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
 * What kind of thing a niche is made of.
 *
 * `Pattern` is what this file was written for and is still the only kind on a target whose values are
 * machine words: a range of integers the scrutinee cannot hold, and every constructor outside the
 * range is one of them.
 *
 * `Absent` is what a target whose values are *host* values has instead. A JS value is not a bit
 * pattern, so the integer patterns the search finds are not what one leaves free - what it leaves free
 * is `null`, which is one pattern, available on everything that is not itself nullable. That is why it
 * carries no range: it is available or it is not, `fits` answers `count <= 1`, and `Maybe(T)` folds
 * where `Maybe(Maybe(T))` and `Result(a, b)` decline.
 *
 * Both kinds feed one search, which is the property worth preserving. A scalarized record on JS is a
 * `number` and donates a `Pattern` niche out of its spare bits exactly as a native one does, so
 * `Maybe(OpenFlags)` folding into a spare bit while `Maybe(Person)` folds into `null` is two answers
 * from one algorithm rather than two algorithms.
 */
enum class NicheKind : U8 {
    Pattern,
    Absent,
};

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
 * `bytes == 0` means no niche. A target whose values are host values rather than words answers with
 * `NicheKind::Absent` instead, for which none of the range below means anything - see that enum.
 */
struct Niche {
    // Where the scrutinee word sits inside the representation that exposes this niche, and how wide
    // it is. A niche found in a nested field is republished by its parent with the offset adjusted,
    // which is what "recursively through nested fields" means in practice.
    U32 offset = 0;
    U8 bytes = 0;

    // `Absent` carries no range and no offset - it is the whole value that is or is not there.
    NicheKind kind = NicheKind::Pattern;

    // The inclusive range of patterns a valid value can produce. Everything outside it is free.
    U64 validStart = 0;
    U64 validEnd = 0;

    bool exists() const { return bytes != 0; }
    bool isAbsent() const { return kind == NicheKind::Absent; }

    // How many patterns are free below and above the valid range. Saturating, because a 64-bit word
    // whose valid range is empty has 2^64 free patterns and no count can hold that - and every
    // caller only ever compares the answer against a constructor count.
    U64 freeBelow() const { return validStart; }
    U64 freeAbove() const;

    // Whether this niche can distinguish `count` alternatives on top of the value itself. One, for
    // `Absent`, which is exactly the `Nothing`/`Just` shape and is the whole of why `Maybe(T)` folds
    // on a host target and `Result(a, b)` does not.
    bool fits(U64 count) const {
        if(!exists()) return false;
        if(isAbsent()) return count <= 1;
        return freeBelow() >= count || freeAbove() >= count;
    }
};

/*
 * Where one field of an aggregate physically sits.
 *
 * `bitWidth == 0` is the ordinary case: the field owns its storage, starting at `offset`, and is
 * read and written as a whole. A non-zero `bitWidth` is a co-packed field - `@bits(n)`, or a target
 * that packed two booleans into one byte - and then the access is a load of the `wordBytes`-wide
 * word at `offset`, a shift by `bitOffset` and a mask, rather than a load of the field itself.
 *
 * Two fields whose `offset`/`wordBytes` name overlapping storage do *not* alias each other for
 * exclusivity, and that is a deliberate reversal of what this comment used to say. The classic C
 * bitfield hazard - two independent write-backs racing to merge into one word, the second clobbering
 * the first - needs the second commit to have read the word before the first one wrote it. A
 * write-back reads the word at commit time, and two commits are ordered, so it never does.
 * `sharesStorageWith` is left because the aliasing question is real for anything that caches a word
 * across a commit, and nothing in the compiler is allowed to.
 */
struct FieldRepr {
    TypePtr type = nullptr;
    U32 offset = 0;
    U8 wordBytes = 0;
    U8 bitOffset = 0;
    U8 bitWidth = 0;

    /*
     * The storage here is an owning pointer to a `type` rather than a `type` - `Field::boxed`, from
     * either a written `@box` or the compiler's automatic indirection.
     *
     * Carried into the Repr rather than looked up again because a Repr is handed out on its own: a
     * niche-folded record and a newtype both *copy* their content's field list, so by the time
     * anything reads this list there is no tuple left to ask. What reads it is every walk that would
     * otherwise recurse into `type` - which for a recursive declaration is the regress the box exists
     * to cut.
     */
    bool boxed = false;

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
 *
 * `Bits` is the case in between, and the one that needs no niche to exist. The payload keeps its own
 * layout at offset zero and the tag is a bit range in the same word, immediately above it: `A(U8)`
 * against `B(U8)` is one byte of payload and one bit of tag, so the whole record is two bytes rather
 * than the eight a four-byte tag word in front of a byte cost. Reading it is a shift and a mask and
 * writing it a read-modify-write, exactly as for a co-packed field - see scalarizeSum.
 */
enum class DiscriminantKind : U8 {
    None,
    Word,
    Niche,
    Bits,
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

    /*
     * Non-zero where the whole of this representation is one integer of that many bits.
     *
     * True of an integer and of a payload-free sum, which have always been scalars, and now also of a
     * record whose every field is narrow: `data Flags {a: Bool, b: Bool}` is two bits, and a record
     * containing one co-packs it into a word of its own rather than giving it a byte. That is whole-
     * record scalarization, and the reason it is one number here is that everything else follows from
     * it - `isNarrowRepr` decides the reference ABI by it, `packBits` places it inside a parent by it,
     * and the niche search reads the patterns above it.
     *
     * It is *this target's* answer, which is what lets a target decline. `valueWidth` in resolve says
     * which aggregates have a scalar form at all, from the logical type alone so that the answer is
     * the same everywhere; a target whose budget is narrower lays the same record out as an ordinary
     * aggregate and leaves this zero, and both sides of a call read this table rather than that
     * predicate, so they cannot disagree about what a `&` of it is.
     */
    U32 scalarBits = 0;

    // What indexing homogeneous storage advances by. `alignUp(size, align)` for everything today,
    // and explicit because a packed element or a target ABI may choose otherwise.
    U32 stride = 0;

    // The niche this representation exposes to whatever contains it, if any.
    Niche niche;

    // Aggregates: one entry per field of the tuple, in field order. Empty for a scalar.
    Array<FieldRepr> fields;

    // Sum types. `discriminantBytes` is the storage the tag is read and written through - the whole
    // word, for `Bits`, since a bit range is reached by loading the word that contains it.
    DiscriminantKind discriminant = DiscriminantKind::None;
    U32 payloadOffset = 0;
    U32 discriminantBytes = 0;
    NicheEncoding encoding;

    // `Bits` only: where in that word the tag sits, and how wide it is. The offset is where the
    // widest payload ends, so that a payload is never disturbed by a tag write and a copy of a whole
    // payload is never able to disturb the tag.
    U32 discriminantBitOffset = 0;
    U32 discriminantBits = 0;

    // Set where the type has no layout on this target because it is not concrete. A generic body
    // reads what it needs out of the environment its caller passed instead.
    bool opaque = false;

    bool isNicheFolded() const { return discriminant == DiscriminantKind::Niche; }
    bool isBitTagged() const { return discriminant == DiscriminantKind::Bits; }
};

/*
 * Whether a `&T` for this representation is an address plus a shift rather than an address -
 * Design.md's tier 2, asked of the target that is emitting.
 *
 * A scalar that does not fill its own storage is one: there are bits of the word it does not own, so
 * it may have been co-packed into a neighbour's and a reference to it has to say where it starts. A
 * scalar that fills its storage cannot have been, so its shift is *provably* zero and `&Int` stays
 * exactly the address it always was.
 *
 * Both sides of a call answer this from the same table - a callee was compiled once and has only the
 * pointee type - which is what makes it an ABI rather than a per-call-site decision.
 */
inline bool isNarrowRepr(const Repr& repr) {
    return repr.scalarBits != 0 && repr.scalarBits < repr.size * 8;
}

/*
 * Everything one target decides differently, and nothing else.
 *
 * A `ReprTable` is this plus a cache, so the difference between the native and the JS families is
 * entirely the object handed to the table. That is what keeps the search - bottom-up over the type
 * graph, niche first, discriminant word as the fallback - one implementation with two answers rather
 * than two implementations that have to be kept saying the same thing.
 */
/*
 * Which family of machine a target is, for the one decision that depends on the family rather than
 * on any number in the table below.
 *
 * That decision is inlining, in compiler/opt: what an aggregate *costs* when it is not held in a
 * register differs in kind rather than in degree between the two. Natively it is bytes at an
 * address the frame already has, so removing a construction saves stores; on a managed host it is
 * an allocation, a hidden class and work for a collector, which is why opt_inline.cpp is willing to
 * inline a larger callee there to make one go away. Code size cuts the other way for the same
 * reason - the host has its own inlining budget and a big function spends it.
 *
 * A field rather than a reading of `CompileMode`, so that the rule stays "a target is chosen by
 * whoever emits" - see opt.h. The optimizer is handed a target and asks it, exactly as it asks it
 * for every layout question.
 */
enum class TargetFamily: U8 {
    Native,  /// Machine code. An aggregate is bytes, and the code below this is an optimizing backend.
    Managed, /// A host runtime. An aggregate is a collected object, and emitted code is source text.
};

struct ReprTarget {
    TargetFamily family = TargetFamily::Native;

    // The width and alignment of an address. JS has no addresses, and answers with what its own
    // reference values cost so that a size computed over a pointer-shaped field is not zero.
    U32 pointerSize = 8;
    U32 pointerAlign = 8;

    // The widest integer a value can occupy without being split. 64 on native; 53 on JS, which is
    // the point at which a host `number` stops representing consecutive integers - see Design.md's
    // "JS target packing", where this single number is the whole of that section's budget.
    U32 integerBits = 64;

    /*
     * How many low bits of a machine word an address actually uses, leaving the rest free to carry
     * something alongside it.
     *
     * What carries something is Design.md's tier 2: a `&T` for a narrow `T` is an address plus the
     * shift of the field within the unit it names, and this is where the shift goes. Five bits at
     * most - a field never straddles the natural storage unit of its own width, so its offset within
     * that unit is bounded by the unit - against sixteen free here.
     *
     * Forty-eight is what every 64-bit target this compiler emits for actually implements: x86-64
     * without five-level paging and AArch64 with a 48-bit VA both leave the top sixteen bits of a
     * user address unused, which is the same assumption every tagged-pointer runtime makes. It is a
     * field rather than a constant because a target that does not have the room has to say so, and
     * what it would get instead is the two-word form - one register more per narrow borrow, and no
     * other difference.
     */
    U32 addressBits = 48;

    /*
     * The widest word this target may co-pack a run of fields into, or represent a whole record as.
     *
     * Bounded above by `kMaxPackBits` in resolve/type.h, which is the same number as a *language*
     * rule: `valueWidth` decides which records have a scalar form without knowing which target is
     * emitting, so a target may be narrower than the budget and none may be wider. A narrower one
     * packs fewer runs and scalarizes fewer records, and `Repr::scalarBits` is where it says so.
     *
     * This is the number a target with wider registers than its integers raises. An SSE target would
     * set it to 128 and get the placement half for free - `packBits` is written in bits and does not
     * care - but not the access half: the read-modify-write a packed field is lowered to computes in
     * a 64-bit value. So raising it is a layout decision that a wider access path has to be able to
     * serve, which is why it is separate from `integerBits`: that one is about what a *value* of this
     * target can hold, and this one about what a *load* of it can carry.
     */
    U32 maxPackBits = 64;

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
     * Co-packing several small fields into one word.
     *
     * On, for native. Reading a packed field is a load of its containing word plus a shift and a
     * mask, and writing one is a read-modify-write; both are intercepted at the load and the store
     * exactly the way a folded tag is.
     *
     * What took longer to settle was what happens to `&h.length`, since a packed field has no
     * address to hand over. The answer this flag depends on is *not* a whole-program "some field of
     * this type has its address taken" requirement selecting an unpacked variant - that would make a
     * record's layout, and the cost of every access to it, a function of code its author never
     * reads. It is Design.md's tier split, decided at the borrow: a borrow the callee does not
     * retain materializes into a temporary and is written back after the call, and one that escapes
     * is reported (and, once PropertyWitness lands, carries the witness instead). See
     * `packCandidate` in resolve/type.h for the half of that decision this file is bound by.
     *
     * The contract with resolve runs one way. Resolve names the fields a target *may* pack, from the
     * logical type alone so that the answer is the same on every target; this may pack fewer of them
     * and may never pack one resolve did not name. Packing a field resolve thinks is addressable is
     * the miscompile the tiering exists to prevent.
     */
    bool packFields = false;

    /*
     * A sum's tag as a few bits above its payload, rather than a word in front of it - see
     * scalarizeSum.
     *
     * Separate from `packFields`, which it otherwise reads as a special case of, because it asks
     * something of the target that co-packing does not: the payload and the tag have to share one
     * word that a single load can reach, since that is what makes the tag a shift and a mask of
     * something already in a register.
     *
     * Off for JS, where a payload is not a word. A record that stays an object there has one property
     * per field, so "the bits above the payload" names nothing - and turning it on anyway would give
     * every sum a Repr whose size and discriminant the backend contradicts, and a niche made of
     * patterns of a word that does not exist. What JS wants from this shape is the *other* half -
     * a sum whose payload is already one number becoming one number - and that is whole-record
     * scalarization applied to sums rather than this.
     */
    bool bitTagSums = false;

    /*
     * Representing a whole aggregate as one integer, rather than co-packing a run of fields inside
     * one that stays an aggregate.
     *
     * Separate from `packFields` because on JS the two are different features with different sizes.
     * There, what this removes is not bytes but the *object*: a scalarized record has no allocation,
     * no hidden class, nothing for the collector to trace, a copy that is a register move, `===` for
     * equality, value semantics as a `Map` key, and an `Int32Array` for an array of it. Measured
     * against the same record as an object it is 95% less memory and 15x construction, where
     * co-packing inside a surviving object is 42% and 1.4x - so they are worth turning on separately
     * even on a target that ends up wanting both.
     *
     * On native they are two halves of one thing and both are on. The search is shared either way:
     * `scalarLayout` in resolve says which aggregates have a scalar form at all, from the logical
     * type alone, and this says whether this target takes it.
     */
    bool scalarizeRecords = false;

    /*
     * Folding a sum type's discriminant into a niche its payload leaves free.
     *
     * On, for native. A folded tag is not stored anywhere: reading it compares the payload's own
     * bits against the range its type can reach, and writing it is a store of one impossible pattern
     * or - for the constructor that owns the payload - nothing at all. Both are intercepted at the
     * load and the store rather than in the place walk, since a place resolves to an address and a
     * folded tag has none. See decodeNicheTag and encodeNicheTag in resolve/lower.cpp.
     *
     * Off for JS, and not because the lowering is missing there but because the *niche* is the wrong
     * one. A JS value is not a bit pattern, so the integer patterns this search finds are not what a
     * host value leaves free - what it leaves free is `null`, which is one pattern available on
     * every type that is not itself nullable. Folding on JS therefore wants a niche kind of its own
     * plus a value representation where a folded record *is* its payload, which is what makes
     * `Maybe(Id)` the `number | null` Design.md asks for rather than an object with a tag.
     */
    bool foldNiches = false;

    /*
     * Whether a value of this target that is not a number has `null` to spare.
     *
     * This is the JS half of the sentence above. A host value is not a bit pattern, so the ranges the
     * search finds over a *word* mean nothing for one: a borrow's "pattern zero is unreachable" is a
     * statement about an address, and there are no addresses here. What every non-nullable host value
     * does leave free is `null`, and that is one pattern rather than a range - see NicheKind::Absent.
     *
     * So on a target that sets this, a representation keeps the niche the search found for it exactly
     * when its value really is a number, which is `scalarBits != 0`, and takes `Absent` otherwise.
     * That split is the whole of the flag, and it is what makes `Maybe(Flags)` one number with the tag
     * in a spare bit while `Maybe(Person)` is `Person | null` - the two answers Design.md asks for,
     * out of one search.
     *
     * A raw pointer is excluded: `null()` and `isNull` are ordinary Native intrinsics written against
     * it, so claiming its absent value would break the allocator exactly the way claiming its zero
     * would on native. Same exclusion, same reason, stated in the target's own terms.
     */
    bool absentNiche = false;
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
    ReprTable(GlobalBase global, const ReprTarget& target);
    ~ReprTable();

    ReprTable(const ReprTable&) = delete;
    ReprTable& operator = (const ReprTable&) = delete;

    const Repr& of(TypePtr type, ReprRequirements requirements = {});

    /*
     * The half of the ABI contract with resolve that a target could break.
     *
     * `isDirectType` in resolve/type.h is a decision rather than an observation, and the contract runs
     * one way: resolve says which types are carried as a copy in a register, and a target's calling
     * convention is bound by that answer rather than free to reach its own. It matters because it is
     * load-bearing for a *diagnostic* - `arrivesAsCopy` decides whether a `return` parameter has
     * anything to root a borrow in, and the answer has to be the same for every backend or the set of
     * accepted programs is not a property of the language.
     *
     * The direction that would actually go wrong is a target passing a type resolve calls a *memory*
     * type in registers - SysV's two-eightbyte rule would do exactly that to `data Point {x: Int, y:
     * Int}`, and a `return p: Point` that had been accepted would then be rooting a borrow in a copy.
     * Nothing here can check that yet, because no convention classifier exists; when one lands it
     * belongs here, next to this.
     *
     * What is checkable now is the other end: a type resolve calls direct has to actually fit in a
     * register on this target. That catches the shape of mistake that is easy to make while the
     * predicate is computed from the type's *kind* - a kind admitted to it whose representation turns
     * out to be wider than a word.
     */
    void checkAbiContract(TypePtr type, const Repr& repr) const;

    U32 sizeOf(TypePtr type) { return of(type).size; }
    U32 alignOf(TypePtr type) { return of(type).align; }
    U32 strideOf(TypePtr type) { return of(type).stride; }

    // The placement of one field of a tuple, or of the content tuple of a single-constructor record.
    // Null when the type has no such field, which is a compiler bug rather than a program error.
    const FieldRepr* fieldOf(TypePtr type, U16 index);

    /*
     * Whether any word of this representation has bits that no field owns - and therefore whether
     * fresh storage for it has to be zeroed before anything reads a niche out of it.
     *
     * The niche a packed word publishes *is* those bits: two `Bool`s in a byte leave patterns 4..255
     * free, and `Maybe(Flags)` spends one of them on `Nothing`. But a packed field is written with a
     * read-modify-write, which by construction preserves everything it does not own - so the bits the
     * niche is made of are whatever the storage happened to contain, and a `Just` built in a fresh
     * frame slot can read back as a `Nothing`.
     *
     * That is why this is asked at the *allocation* rather than fixed at the write. A write cannot fix
     * it: the callee holding a `&Bool` into someone's byte knows its own field's width and unit, and
     * has no idea which of the remaining bits belong to a neighbour and which to nobody. The one place
     * that knows is the one that created the storage, which is also the only place it is free.
     *
     * True through nested fields, since a niche found in one is republished by its parent - with the
     * same depth limit `valueWidth` carries, and for the same reason: a type that reaches itself by
     * inline containment has been reported already, and this still has to return.
     */
    bool hasPaddedWord(TypePtr type, U32 depth = 0);

    GlobalBase global;
    ReprTarget target;

    /*
     * What an owning indirection occupies in whatever holds it.
     *
     * A `@box` field and a boxed constructor payload (`Field::boxed`, `Constructor::boxed`) are the
     * same thing physically: one non-null pointer, whatever is on the other end of it. So there is
     * one of these per table rather than one per boxed edge, and the target is what decides how wide
     * a pointer is.
     *
     * The niche is the load-bearing half. The box is never null - it is written at construction and
     * released at teardown, and nothing can observe it in between - so pattern zero is free, and that
     * is what makes `Maybe(Tree)` one word with `Nothing == 0` by the ordinary niche search, with
     * nothing written against `Maybe`.
     */
    Repr indirection;

private:
    // The representation of one member of an aggregate: the member's own, or the box, where the edge
    // to it is an indirection. Every layout question about a boxed edge is a question about the
    // pointer, which is the whole point of a box - what is on the other end has no bearing on the
    // size of what holds it.
    const Repr& memberOf(TypePtr type, bool boxed) { return boxed ? indirection : of(type); }

    void compute(TypePtr type, Repr& into);
    void computeTuple(TupType& tuple, Repr& into);
    bool scalarizeTuple(TupType& tuple, Repr& into);
    void placementOrder(TupType& tuple, Array<U16>& into);
    Size packWord(TupType& tuple, Repr& into, Buffer<const U16> order, Size first, U32& size,
                  U32& alignment);
    void computeRecord(RecordType& record, Repr& into);
    bool foldNiche(RecordType& record, Repr& into);
    bool scalarizeSum(RecordType& record, Repr& into, U32 payloadSize, U32 payloadAlign);

    /*
     * Whether a value of this type can live inside a packed word *on this target*.
     *
     * The bits always fit - `valueWidth` said so, and this is only ever asked of something it called
     * narrow. What this asks is the other half: a field is moved in and out of the word as a value of
     * its own type, and Design.md's bit-width rule says which type that is. A load of a `@bits(20)
     * U64` widens to `U64`, so what comes out of the word is a `U64`, and a target whose values hold
     * fewer bits than that cannot produce one.
     *
     * Native answers yes to everything, since `integerBits` is 64 there and no integer is wider. It
     * is the JS target that has something to say: a value there holds 53 bits, and an integer whose
     * canonical width is 64 is a `bigint` - a different host type, with different operators, that a
     * shift and a mask of a `number` cannot produce. Packing one would emit `word & ~mask | value`
     * over a mixed pair and fail at run time rather than silently.
     *
     * Asked of the *canonical* width rather than the refined one for the same reason the JS backend's
     * `isLong` is: a refinement and the type it refines are the same host type, and the refinement is
     * exactly the thing whose logical width would otherwise say yes.
     */
    bool packableHere(TypePtr type, U32 depth = 0);

    U32 naturalBytes(U32 bits) const;
    Niche intNiche(const IntType& integer, U32 offset) const;
    Niche addressNiche(U32 offset) const;
    void hostNiche(TypePtr type, Repr& into);

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
