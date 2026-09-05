#pragma once

#include "../../resolve_mir/type.h"
#include "../../compiler/settings.h"

namespace ssa {

/*
 * Physical representation for a target whose values are **bytes at an address** - the native half of
 * Analysis-Borrows-Revised §1.3's third layer.
 *
 * ```text
 * repr(logical type, target)     -> the bytes
 * manifest(occurrence, target)   -> which repr, and how the value is carried
 * ```
 *
 * That split is the whole shape of this file. A `Repr` is a function of the logical type and the
 * machine and of nothing else; `manifest.h` is the layer over it that reads a *contract* - which
 * record a view manifests as, whether a loan is an address or the bytes at one - and there is
 * exactly one of each. Everything that used to derive an offset, a size or a stride asks one of
 * them, so "how wide is a `Point`" has a single answer above the backends.
 *
 * ## Why this is a family rather than a parameter
 *
 * The shipping compiler's `compiler/repr` computes both targets' layouts from one `Repr` with a
 * `ReprTarget` of numbers and flags, and the strain shows in the struct: `NicheKind::Absent` marks a
 * niche whose `offset`, `bytes` and range all mean nothing, and a JS `ReprTarget` carries
 * `pointerSize = 8` for a machine with no addresses purely so that a size computed over a
 * pointer-shaped field is not zero. Those are two readings of one struct, which is a sum type
 * spelled as a flag record.
 *
 * So the *answer* is per value model. This file is the model where a value is bytes: every number in
 * `Repr` below is a statement about addressable storage. A host target whose values are host values
 * answers with `JsRep` instead (Analysis-IR-JS.md §3.2) - `Number`, `GcObject{shape}`, `Nullable`,
 * `Parts`, `Cell` - which is a different type and not this one with different numbers.
 *
 * ## What is deliberately *not* per family
 *
 * Three things, and keeping them out of here is what stops two tables becoming two compilers that
 * have to be kept saying the same thing:
 *
 *  - **the key.** A manifestation is selected from a whole `mir::Occurrence` - logical type,
 *    contract, interior shape - on every target. A planner keyed on less can select an owner where
 *    the contract said view, which `manifestNative` has already made and fixed once.
 *  - **what the language decides.** `RecordType::pin` is `@layout(c)`; `RecordType::layout` is
 *    whether a record is an enum, a single constructor or a sum; the view table pairs an owner with
 *    its window; the containment cut is one diagnostic against source. All of them live on the
 *    logical type in `resolve_mir`, so that every family reads one statement.
 *  - **the cost model.** What an aggregate costs, and whether removing one is worth inlining for, is
 *    a question about the machine and not about a layout. It does not belong on `NativeTarget` and
 *    must not be reachable through this table - which is what a `TargetFamily` field on a repr target
 *    turns into, and the shipping compiler has four optimizer passes reading one.
 */

/*
 * The widest word a run of co-packed fields may occupy, as the **language** rather than as a target.
 *
 * A target may be narrower than this - `NativeTarget::maxPackBits` is where one says so - and none
 * may be wider, because two numbers below are stated in terms of it: the read-modify-write a packed
 * access lowers to computes in a 64-bit value, and `storageBitsFor` stops at 64. Raising it is a
 * layout decision that a wider access path then has to be able to serve.
 */
static constexpr U32 kMaxPackBits = 64;

/*
 * The low `bits` set, saturating at the whole word.
 *
 * The value mask of a narrow field, the top of a niche's valid range, and the constant every packed
 * read and write is built out of - one function because `1 << 64` is undefined and a second spelling
 * of this is a second place that gets that wrong. Here rather than beside either the layout or the
 * lowering, because both of them compute it and a mask the layout published has to be the mask the
 * access applies.
 */
inline U64 lowMask(U32 bits) { return bits >= 64 ? ~U64(0) : (U64(1) << bits) - 1; }

/*
 * The bit patterns one word of a representation can never legally hold.
 *
 * This is what makes discriminant folding a general algorithm rather than one optimization per type
 * pair: a niche is declared once, by the type that has one, and every sum built over that type
 * benefits without anyone touching the sum. `Maybe`, `Outcome` and a library-defined wrapper all
 * fold through one search because none of them knows what a niche is.
 *
 * One contiguous valid range rather than a general pattern set, because the three shapes that matter
 * all fit it: a non-null address (`validStart == 1`, so pattern 0 is free), a `@bits(n)` integer in a
 * wider word (`validEnd == 2^n - 1`, so everything above is free), and a bounded enum discriminant
 * (`validEnd` is the highest constructor number).
 *
 * `bytes == 0` means no niche. **The search runs whether or not this target folds**, because a niche
 * is a fact about a type and folding is a decision about a sum - which is what lets the search be
 * reviewed and asserted before the lowering that spends it exists.
 */
struct Niche {
    // Where the scrutinee word sits inside the representation exposing this niche, and how wide it
    // is. A niche found in a nested field is republished by its parent with the offset adjusted,
    // which is what "recursively through nested fields" means in practice.
    U32 offset = 0;
    U8 bytes = 0;

    // The inclusive range of patterns a valid value can produce. Everything outside it is free.
    U64 validStart = 0;
    U64 validEnd = 0;

    bool exists() const { return bytes != 0; }

    U64 freeBelow() const { return validStart; }

    // Saturating, because a word whose valid range is empty has more free patterns than a count can
    // hold - and every caller only ever compares the answer against a constructor count.
    U64 freeAbove() const;

    bool fits(U64 count) const {
        if(!exists()) return false;
        return freeBelow() >= count || freeAbove() >= count;
    }
};

/*
 * Where one field of a record physically sits.
 *
 * `bitWidth == 0` is the ordinary case: the field owns its storage from `offset` and is read and
 * written whole. A non-zero `bitWidth` is a **co-packed** field - two booleans sharing a byte, a
 * `@bits(n)` integer beside its neighbour - and then an access is a load of the `wordBytes`-wide
 * word at `offset`, a shift by `bitOffset` and a mask, rather than a load of the field itself.
 */
struct FieldRepr {
    /*
     * The whole occurrence and not the type, because a field's bytes are decided by its contract as
     * much as by what it names: `data Held {at: 'Int}` is a *pointer*, and a `FieldRepr` carrying
     * `I32` would describe four bytes where the record has eight. It is the same triple
     * `layoutRecord` measured the field with, kept so that nothing has to re-derive it.
     */
    mir::Occurrence occurrence;
    U32 offset = 0;
    U8 wordBytes = 0;
    U8 bitOffset = 0;
    U8 bitWidth = 0;

    /*
     * Which bit of the record's provenance word says whether this box is the program's to hand
     * back - `Repr::boxFlagsOffset`, and `kNoBoxFlag` on every field that is not a box.
     *
     * Only in the mode that stores one. A build whose allocator answers `isHeapAddress` recognizes
     * its own allocations from the address, so no record carries a word and every boxed field here
     * holds `kNoBoxFlag` - which is what makes the two modes one layout walk with a switch in it
     * rather than two layouts.
     */
    static constexpr U8 kNoBoxFlag = 0xff;
    U8 boxFlagBit = kNoBoxFlag;

    bool isPacked() const { return bitWidth != 0; }
    bool hasBoxFlag() const { return boxFlagBit != kNoBoxFlag; }
};

/*
 * How a record says which constructor it holds - four answers, in decreasing order of what the
 * layout had to spend.
 *
 * `None` is a single-constructor record, which has nothing to say. `Word` is a separate integer in
 * front of the payload - the form every sum had before any of the rest existed, and the one every
 * sum still has where nothing better was found. `Bits` is a few bits *above* the payload's own,
 * inside the same word: the arms are narrow, so what the tag costs is the bits nobody was using
 * rather than a byte and the alignment behind it. `Niche` is the folded form, which costs nothing at
 * all: the discriminant **is** a pattern of the payload's own storage that no payload value can
 * produce, so the record is exactly its widest constructor and reading the tag is a range test.
 *
 * A kind rather than "`discriminantBytes` is zero", because the two are different statements and
 * two of these make both: a folded record stores no tag *and* has a constructor to report, and a
 * bit-tagged one has a `discriminantBytes` that is the **whole record** rather than a field in it.
 * What reads a tag asks this first, which is what keeps "where is it" and "how is it spelled" one
 * question with one answer.
 */
enum class Discriminant: U8 {
    None,
    Word,
    Bits,
    Niche,
};

/*
 * The niche encoding of a record's discriminant.
 *
 * One constructor keeps the payload untouched and is recognized by the scrutinee word being *inside*
 * the valid range; every other constructor is one pattern outside it. `patternOf` is the encoding
 * and the decode is its inverse, and they are stated here rather than at the two lowering sites so
 * that a read and a write cannot disagree about which pattern meant what.
 */
struct NicheEncoding {
    Niche niche;

    // The constructor whose payload occupies the storage. Every other constructor is one pattern.
    U16 payloadConstructor = 0;

    /*
     * The first pattern handed out, and whether they run upwards from it.
     *
     * Taken from *below* the valid range where there is room there, which is what makes `Nothing`
     * zero for a `Maybe` over a non-null address - a plain nullable pointer, the representation a C
     * programmer would have written and the one a debugger can read - and from above it otherwise.
     */
    U64 firstPattern = 0;
    bool ascending = true;

    // Which pattern stands for this constructor. The non-payload constructors take the patterns in
    // index order, so the payload one has to be skipped over on the way - which is the whole of the
    // arithmetic and the reason it is written once.
    U64 patternOf(U16 constructor) const;
};

/*
 * One logical type's bytes on this machine.
 *
 * Produced by `reprOf` and immutable afterwards: an entry is never rewritten, because a `Repr`
 * computed while a dependent one was being built would have to be invalidated and nothing here is
 * arranged to notice.
 */
struct Repr {
    /*
     * Where the last field ends, and **not** rounded up to the alignment.
     *
     * The two being separate numbers is what lets a container fit in two words: a record's trailing
     * padding belongs to whoever contains it, so a parent places its next field at the child's
     * `size` and lands inside what the child would have wasted. What a *second* value beside it
     * starts at is `stride`.
     */
    U32 size = 0;
    U32 align = 1;

    // `alignUp(size, align)` - what indexing homogeneous storage advances by, and the one number two
    // elements of an array may never disagree about.
    U32 stride = 0;

    /*
     * Non-zero where the whole of this representation is one integer of that many bits.
     *
     * True of an integer and of a payload-free sum, and - on a target that takes it - of a record
     * whose every field is narrow. It is **this target's** answer, which is what lets a target
     * decline: which aggregates have a scalar form at all is a question about the logical type, so
     * that the answer is the same everywhere, and a target whose budget is narrower lays the same
     * record out as an ordinary aggregate and leaves this zero.
     */
    U32 scalarBits = 0;

    // The niche this representation exposes to whatever contains it, if any.
    Niche niche;

    // One entry per field of the record, in field order, and empty for every kind that has none. A
    // sum's arms share their storage, so two entries may name overlapping bytes.
    DeclList<FieldRepr, false> fields;

    /*
     * How this record's constructor is told apart, and - for the one form that stores it - where.
     *
     * A stored tag sits at offset zero and before every payload, which is a decision rather than an
     * observation: what reads a tag is a match, and a match on a value whose payload it has proved
     * nothing about may only touch bytes every arm agrees are the tag. Putting it first makes that
     * the same address for every arm and for every instantiation.
     *
     * `Niche` stores nothing, so `discriminantBytes` is zero there and `encoding` is the whole
     * answer. `discriminantBytes` is therefore never the test for "does this have a tag" - the kind
     * is, and a folded record is exactly the case the two readings disagree about.
     */
    Discriminant discriminant = Discriminant::None;
    U32 discriminantOffset = 0;
    U32 discriminantBytes = 0;

    /*
     * Which bits of that word the tag actually is.
     *
     * Set for every kind that stores one, so that a reader composing a shift onto a tag has one rule
     * rather than a case per kind: a `Word` tag is the whole word from bit zero, an enum-layout
     * record's is its own value bits from bit zero, and a `Bits` tag is `discriminantBits` bits at
     * `discriminantBitOffset`, above everything the payload spends.
     *
     * That uniformity is what a co-packed sum needs. `p.state` where `state` is a bit-tagged sum
     * sharing a word reads the tag at the sum's shift *plus* this offset, which is the same
     * composition an ordinary field of a scalarized record gets - and a rule with an exception in it
     * is one the composition would have to test for.
     */
    U32 discriminantBitOffset = 0;
    U32 discriminantBits = 0;

    NicheEncoding encoding;

    // Where the constructors' shared payload area starts, past the discriminant. Zero for a folded
    // record, whose payload *is* the record.
    U32 payloadOffset = 0;

    /*
     * The **box provenance word** - one bit per boxed field, saying whether that allocation is the
     * program's to hand back.
     *
     * `boxFlagsBytes` is zero for every record with no boxed field, and for *every* record in a
     * build whose allocator answers `isHeapAddress`: there the question is asked of the address and
     * nothing is stored. `known.def`'s `isHeapAddress` is where the two modes are argued.
     *
     * Placed after every field rather than among them, and that is deliberate. The bits are the
     * compiler's own bookkeeping and no field index names them, so putting them at the end keeps
     * every offset this walk computed the offset it would have computed without them - a record's
     * layout in the two modes differs by a suffix and in no other way, which is what makes the pair
     * of fixtures comparable line by line.
     *
     * A sum's arms share their payload and their boxed fields do **not** share bits: a bit per
     * *field* of the whole record, so the word is as wide as the record's total box count rather
     * than the widest arm's. It costs at most a byte more than the packed alternative and it means
     * an arm's release reads the bit its own field index names, with nothing to remap.
     */
    U32 boxFlagsOffset = 0;
    U8 boxFlagsBytes = 0;

    // Set where the type has no layout on this target because it is not concrete. A generic body
    // reads what it needs out of the environment its caller supplied instead.
    bool opaque = false;

    /*
     * Whether a `&` of this representation is an address plus a shift rather than an address.
     *
     * A value that does not fill its own storage has bits of the word it does not own, so it may
     * have been co-packed into a neighbour's and a reference to it has to say where it starts. One
     * that fills its storage cannot have been, so its shift is *provably* zero and `&Int` stays
     * exactly the address it always was.
     *
     * Read off this table at both ends of a call rather than derived at either, which is what makes
     * it an ABI rather than a per-site decision: a callee was compiled once and has only the pointee
     * type.
     */
    bool isNarrow() const { return scalarBits != 0 && scalarBits < size * 8; }

    bool isFolded() const { return discriminant == Discriminant::Niche; }

    // A tag of bits sharing the payload's word - the one kind whose `discriminantBytes` names the
    // whole record rather than a field of it, so a reader that stores or loads a tag has to ask.
    bool isBitTagged() const { return discriminant == Discriminant::Bits; }
};

/*
 * Everything this machine decides differently, and nothing else.
 *
 * A second native target is a different value of this struct and never a second walk, which is what
 * keeps "how wide is a `Size`" one question. What is deliberately absent is any statement about what
 * a value *costs* - see the note at the top of this file.
 */
struct NativeTarget {
    /*
     * Which end of a word the target puts the low byte at.
     *
     * `LittleEndian` for every architecture this compiler emits, and a field rather than an
     * assumption because it is a fact about the *reader*: a program that got it wrong would produce
     * an image whose tables read as enormous numbers rather than failing to build.
     */
    ByteOrder byteOrder = LittleEndian;

    /*
     * What an address costs on this machine.
     *
     * These were constants until 2026-09-05, and the reason they stopped being one is the reason
     * `targetAddressBytes` exists: nothing here read the architecture, so a build for x86 or 32-bit
     * ARM was accepted and laid out with eight-byte pointers. `nativeTargetFor` is the one place
     * they are filled, from the same setting the word width comes from, so a `Size` and a pointer
     * cannot come to disagree about how wide the machine is.
     *
     * Not a table's business. `erase.cpp` sizes a compiler-built table's cells at four bytes on
     * purpose and independently of the target's pointer width, so a narrower target changes these
     * and leaves that layout alone.
     */
    U32 pointerSize = 8;
    U32 pointerAlign = 8;

    /*
     * Where a function value's two words are - the layout `mir::Type::Fun` manifests as.
     *
     * Stated once because both ends read them: `emitFunValue` writes the pair and a call through a
     * value reads it back, and a pair of numbers written at two sites is a pair that eventually
     * disagrees. `code` leads because it is the word that is never null - a plain function referenced
     * by name has no environment, and nothing may have to test the second word to find the first.
     *
     * **Below the target fork**, which is the altitude that matters rather than the file. OSSA
     * carries a `fun.value` naming a declaration and an environment operand, and `Analysis-IR-JS.md`
     * is explicit that whether that is "one object or two values at a call" is a question OSSA must
     * not answer. So these are the *native* answer, and JS generation reads the node rather than
     * these bytes.
     *
     * Derivations rather than numbers - a function value is **two words**, not sixteen bytes - and
     * on the target rather than beside the constants they replaced, because `reprOf` reads the size:
     * a layer that had to reach up into the one above it for a size is a layer boundary that is not
     * one.
     */
    U32 funCodeOffset() const { return 0; }
    U32 funEnvOffset() const { return pointerSize; }
    U32 funSize() const { return 2 * pointerSize; }

    /*
     * The widths the language leaves to the machine - `Size`, `USize` and `CodeUnit`.
     *
     * The first genuinely target-dependent number here, and the reason the pair above is still a
     * literal default while this is not: a program *names* these three, so what they are wide is a
     * question the source asks and the two ends of a field layout have to answer the same way.
     */
    IntWidths integers;

    /*
     * How many low bits of a machine word an address actually uses, leaving the rest free to carry
     * something alongside it.
     *
     * Forty-eight is what every 64-bit target this compiler emits for implements: x86-64 without
     * five-level paging and AArch64 with a 48-bit VA both leave the top sixteen bits of a user
     * address unused. A field rather than a constant because a target without the room has to say
     * so - and a 32-bit one has none, which `nativeTargetFor` answers by switching `packFields` off,
     * since a narrow reference is the only thing the room is for.
     */
    U32 addressBits = 48;

    /*
     * Whether an address is known never to be null, and therefore exposes pattern 0 as a niche.
     *
     * A **raw** pointer is excluded wherever this is read, and the exclusion is not a target's:
     * `null()` and `isNull` are ordinary Native intrinsics that the bump allocator is written in
     * terms of, so claiming a `%T`'s zero would break it. Borrows and the indirection the compiler
     * inserts for a recursive type are the ones that are genuinely non-null.
     */
    bool nonNullBorrows = true;

    /*
     * The widest word this target may co-pack a run of fields into, or represent a whole record as.
     *
     * A number of its own rather than a consequence of `integers`, because they are different
     * questions: that one is what a *value* here can hold, and this is what a *load* of one can be
     * taken apart with.
     */
    U32 maxPackBits = 64;

    /*
     * The three features that change a **layout** rather than a number inside one.
     *
     * Switches rather than three unconditional walks, because each of them is a claim the access
     * path has to be able to serve and the pairing has to be visible: turning one off has to give a
     * layout every consumer already handles, and turning one on has to be the same edit as teaching
     * the accesses to spend it.
     *
     *  - `packFields` - a run of narrow fields sharing one word. A packed field has no address, so
     *    its read is a load, a shift and a mask and its write a read-modify-write; `pack.cpp`
     *    is where the pair lives, a `&` of one carries the shift beside the address, and a `%T` of
     *    one is refused.
     *  - `scalarizeRecords` - a whole record as one integer. The same placement applied to the
     *    record rather than to a run inside it, which is what makes `Maybe(Flags)` one byte.
     *  - `foldNiches` - a sum's tag as a pattern its payload cannot produce. The tag read becomes a
     *    range test, the tag write a store of one impossible pattern - or, for the constructor that
     *    owns the payload, nothing at all.
     *  - `bitTagSums` - a sum whose every arm packs, as one integer with the tag above the payload.
     *    The same read and write a co-packed field gets, applied to the discriminant, which is why
     *    it asks nothing of the access path that the first switch did not already buy.
     *
     * The niche **search** runs whichever way the last is set, because a niche is a fact about a
     * type and folding is a decision about a sum. That is what let the search be reviewed and
     * asserted before the lowering that spends it existed.
     */
    bool packFields = true;
    bool scalarizeRecords = true;
    bool foldNiches = true;
    bool bitTagSums = true;
};

// The target this compilation emits for. One function, so that a second reader of the settings
// cannot come to a different answer than the lowering did.
NativeTarget nativeTargetFor(const CompileSettings& settings);

/*
 * What every question below is asked against: the declarations a type is read out of, and the
 * machine whose answers that type defers to.
 *
 * One value rather than parameters at each entry point, because the two travel together everywhere
 * and a site holding only the first would silently get *this compiler's* answer for a width the
 * program left open. Passing the pair makes "I did not have the target here" a build error.
 */
struct Layout {
    DeclBase base;
    NativeTarget target;

    /*
     * The program the declarations belong to, and the context its types are made in.
     *
     * Here because a manifestation is no longer a function of a type alone: an occurrence under a
     * view contract is laid out as the record the view table pairs it with, and reaching that record
     * instantiates it at the owner's arguments. `layoutFor` is the only place either is filled, so a
     * walk cannot be constructed without them.
     */
    Context* context;
    mir::Program* program;
};

// The layout questions this program's target answers, with the declarations they are answered
// against - `nativeTargetFor`'s own reason, one field wider.
Layout layoutFor(Context& context, mir::Program& program);

/*
 * Whether this build stores a provenance bit per boxed field - the two compilation modes of
 * `known.def`'s `isHeapAddress`, asked as one question.
 *
 * False where the selected allocator answers `isHeapAddress`, because then a release site can ask
 * the address whether the allocation is the program's and nothing has to be written down. True
 * otherwise, and every record with a boxed field grows a word.
 *
 * A property of the **program**, settled long before any layout is computed: which provider a build
 * has is decided by `@allocator` and the file-name axis during declaration reading, so this is a
 * constant by the time a `Repr` is asked for and never something two records could disagree about.
 * That is what makes it safe to memoize a record's layout on the record.
 */
bool storesBoxProvenance(Layout layout);

// How wide a value of this integer type actually is on this machine - the one place a `TargetWidth`
// is turned back into a number, and the bottom of the layer the three abstract widths exist for.
U16 intBitsOn(NativeTarget target, const mir::IntType& type);

// How many bytes a value of this many bits is stored in - the width rounded up to one a machine
// addresses and aligns. Not `bits / 8`, and `WideInt` is why: fifty-three bits are not a whole
// number of bytes of anything, and a size taken by division is an alignment nothing can address.
U32 storageBytesFor(U16 bits);

/*
 * **The** native representation of one logical type.
 *
 * Memoized on a record, because the answer is a property of the type and outlives every body that
 * asks - `mir::RecordType::nativeRepr`, which is why a query on a type three deep does not lay out
 * every type below it again. By value rather than by reference: what it carries is a handful of
 * numbers and one list *handle* into the same region, so a copy is words and every caller gets a
 * value whose lifetime it does not have to reason about.
 *
 * `shape` is the interior structure the *occurrence* holding a record carries - `Maybe('v)`, whose
 * payload is a reference where the declaration wrote a member. Null for every value that owns what
 * is inside it, which is every record but the handful a signature qualified; a shaped one is
 * measured each time and never memoized, since the memo is the *type's* answer and a shaped
 * occurrence is not it.
 */
Repr reprOf(Layout layout, DeclPtr<mir::Type> type,
            DeclPtr<mir::OwnershipSchema> shape = nullptr);

/*
 * Whether this type has a word whose spare bits nobody writes.
 *
 * The one thing co-packing asks of the *allocation* rather than of the access. A narrow value that
 * owns its storage is written by a store of the whole width - a `@bits(13)` field is a two-byte
 * store of a masked value - so its high bits are written every time and the niche it publishes is
 * true without anyone arranging it. A **packed** word is written by a read-modify-write that
 * preserves everything outside the field, so the bits above the run are whatever the storage held,
 * and the niche made of them is only true if that was zero.
 *
 * So fresh storage for one of these is zeroed. Asked here rather than at the allocation because it
 * is a question about the layout - which words this type has and how much of each is spent - and
 * the allocation only has to know the answer.
 */
bool hasPaddedWord(Layout layout, DeclPtr<mir::Type> type, U32 depth = 0);

/*
 * How many bits of its storage a value of this type owns, or zero where the question does not apply.
 *
 * Separate from `Repr::size` because they are different questions, and they first disagree without
 * any attribute being written: `WideInt` is fifty-three bits reserved in eight bytes. `@bits(n)` is
 * the same statement written down - `@bits(13) U32` is thirteen bits reserved in two - an enum-layout
 * record owns as many bits as its constructor numbers need, which for `Bool` is one, and a
 * scalarized record owns the span its fields were placed within.
 *
 * A **read of the table** and not a walk beside it, which is what the last of those makes necessary:
 * the number a callee masks a narrow reference to has to be the number the fields were laid out
 * within, and two functions answering it is two mask widths. The shipping compiler keeps them apart
 * on purpose - `valueWidth` in resolve says which types are narrow without knowing which target is
 * emitting, so that a diagnostic about a borrow reads the same on both - and this stage has one
 * family, so the pressure that split them is not here and the table is the whole answer. It comes
 * back the day a value-oriented family does.
 */
inline U32 narrowBitsOf(Layout layout, DeclPtr<mir::Type> type) {
    return reprOf(layout, type).scalarBits;
}

} // namespace ssa
