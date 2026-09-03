#pragma once

#include "intern.h"
#include "region.h"

namespace mir {

/*
 * Forward-declared rather than included, and that is the layering rather than a build trick: an
 * occurrence *names* a logical type and never asks it anything, so `contract.h` has no reason to
 * know what a `Type` holds. `type.h` includes this one, because a record field's declaration carries
 * a contract - see `RecordField` - and the dependency genuinely runs that way.
 */
struct Type;

/*
 * The second of the three layers - Analysis-Borrows-Revised.md §1.2.
 *
 * A `Contract` describes one *occurrence* of a logical type: whether it is owned here or by an
 * external origin, whether access through it is shared or unique, and which access form the
 * declaration asked for. It participates in borrow checking and in function compatibility, and it
 * deliberately does not participate in instance selection - `Type` alone answers that.
 *
 * Kept separate from `Type` rather than being a qualified type, because a qualified type is exactly
 * the mistake this replaces: once `a'String` is a `TypePtr`, unification, substitution, instance
 * lookup and conversion all have to learn to see through it, and each place that forgets is a bug
 * that looks like a type error.
 */

/// Whether the occurrence owns the value, may read it, or has exclusive write access to it.
enum class Access: U8 {
    Owned,
    Shared,
    Unique,
};

/*
 * Who owns a non-owning occurrence's value.
 *
 * `Here` belongs to an owned occurrence and is the only value that pairs with `Access::Owned`.
 * `CallBounded` is a loan that cannot outlive the call or continuation it was granted for - the
 * default for a plain parameter, and what makes a lens able to yield a view of its own frame.
 * `Slot` is a published relationship to an origin set in the enclosing function or record schema,
 * which is what the `'` and `name'` spellings introduce.
 */
enum class Origin: U8 {
    Here,
    CallBounded,
    Slot,
};

/*
 * A source-visible capability, not a layout.
 *
 * `SequenceWindow` is what `[T]` in a parameter asks for: elements may be read, and under
 * `Access::Unique` replaced, but the extent cannot change. Unique access to the whole `Array(T)`
 * owner is `WholeValue` and may resize. Whether either is one pointer, two words or an adapter is
 * the manifestation table's answer and never appears here.
 */
enum class AccessForm: U8 {
    WholeValue,
    SequenceWindow,
};

/*
 * The root occurrence's qualification, in four bytes.
 *
 * Packed rather than three enumerators beside a `U16`, which is what this was and which the compiler
 * laid out as six: an `Occurrence` is stored on every local, every block argument, every node result
 * and every place record, so two bytes of padding here is two bytes on each of those. The accessors
 * are shifts and masks, and the whole thing compares as one word.
 *
 * `slot` keeps its full sixteen bits and is asserted rather than truncated. Silently dropping the
 * high bits of an identity is the failure mode this representation has already had once, in place
 * interning, and a packed field is exactly where it happens again if nobody checks.
 */
// No published origin at this position - the answer for an owned root, for a call-bounded loan, and
// for an interior position the declaration did not bind.
static constexpr U16 kNoOriginSlot = ~U16(0);

struct Contract {
    static constexpr U32 kAccessShift = 0;
    static constexpr U32 kOriginShift = 2;
    static constexpr U32 kFormShift = 4;
    static constexpr U32 kSlotShift = 8;
    static constexpr U32 kSlotMask = 0xffff;

    Contract() = default;

    Contract(Access access, Origin origin, AccessForm form, U16 slot) {
        packed = (U32(access) << kAccessShift) | (U32(origin) << kOriginShift) |
                 (U32(form) << kFormShift) | (U32(slot) << kSlotShift);
    }

    Access access() const { return Access((packed >> kAccessShift) & 3); }
    Origin origin() const { return Origin((packed >> kOriginShift) & 3); }
    AccessForm form() const { return AccessForm((packed >> kFormShift) & 3); }

    /*
     * Which origin set, when `origin` is `Slot`.
     *
     * Numbered within one ownership schema scope - the enclosing function's signature or the
     * enclosing record's fields - in order of first occurrence, and the bare `'` is one of them
     * rather than a reserved index. Analysis-Borrows-Revised.md §2.2 is explicit that there is "no
     * semantic difference beyond which uses are grouped", so a reserved zero would be a slot every
     * signature pays a table entry for and most of them never write.
     */
    U16 slot() const { return U16((packed >> kSlotShift) & kSlotMask); }

    // Which origin set this contract names, or `kNoOriginSlot` where it names none - the reading
    // every caller outside this file wants, since `slot()` is meaningless without the origin beside
    // it and two sites that forgot to check would be two sites reading zero as a set.
    U16 originSlot() const { return origin() == Origin::Slot ? slot() : kNoOriginSlot; }

    bool isOwned() const { return access() == Access::Owned; }

    bool operator == (Contract other) const { return packed == other.packed; }
    bool operator != (Contract other) const { return packed != other.packed; }

    U32 packed = 0;
};

static_assert(sizeof(Contract) == 4, "a Contract is one word, and the layout note above says why");

// An occurrence that owns its value: a fresh result, a plain field, a local holding a call result.
inline Contract ownedContract() { return Contract {}; }

// A loan bounded by the call or continuation that granted it - the default parameter and the
// default yield. Cannot be stored anywhere that outlives the call.
inline Contract borrowedContract(Access access, AccessForm form = AccessForm::WholeValue) {
    return Contract { access, Origin::CallBounded, form, 0 };
}

// A loan through a published origin set - the `'` and `name'` spellings. The slot is numbered
// within the enclosing ownership schema scope; see `Contract::slot`.
inline Contract slotContract(Access access, U16 slot, AccessForm form = AccessForm::WholeValue) {
    return Contract { access, Origin::Slot, form, slot };
}

/*
 * The contract a loan *of this storage* holds - the read half of `Builder::addRead`.
 *
 * The access is the borrow's and the origin is the storage's, and that pairing is the whole of it: a
 * loan of a parameter declared `x: a'T` is still in origin set `a`, because reaching into somebody
 * else's storage does not change whose it is, while a loan of a local this frame owns is bounded by
 * this frame and publishes nothing.
 *
 * `slot` on the result therefore names the slot of the *declaration that granted the loan* - this
 * signature for a parameter, the callee's for a call result. That is not a scope-local name and
 * nothing reads it as one: which origins a value is actually in is the origin set the loan analysis
 * computes, and what the field is for here is that a published loan and a frame-bounded one
 * manifest differently and the difference has to survive to the manifestation table.
 */
inline Contract reborrowedContract(Contract storage, Access access) {
    if(storage.origin() != Origin::Slot) {
        return Contract { access, Origin::CallBounded, storage.form(), 0 };
    }

    return Contract { access, Origin::Slot, storage.form(), storage.slot() };
}


// Declared below, and named here because a request carries one: what a position asks for includes
// what it says about the value's interior.
struct OwnershipSchema;

/*
 * What a position asks the value that arrives in it for - the input half of `readMode`.
 *
 * Not a `Contract`, deliberately. A contract describes an occurrence that *exists*; a request
 * describes one that does not yet, and the two are not the same statement: `let x = c` has no
 * contract of its own until the initializer says what `c` is, which is the whole of
 * `doc/spec/ownership.md`'s "an initializer either produces storage or names storage that already
 * exists". What they share is that both are read from the **destination**, which is the rule that
 * section states and this type is the one place it is written down.
 *
 * `published` is whether the position promised an origin relationship. It exists because it is
 * exactly what decides whether an implicit trivial copy satisfies the position: the spec names two
 * that it does not - a `&` parameter, "whose whole contract is writing through to the same storage,
 * which a copy cannot satisfy", and any external-origin qualifier, where "copying it would erase the
 * very relationship the signature published". Everything else - a plain parameter, a `->` parameter,
 * a `let` of any spelling - is copied where the type permits.
 */
struct Request {
    Access access = Access::Shared;
    bool published = false;

    /*
     * The origin set the position named, or `kNoOriginSlot` where it named none.
     *
     * A **separate** question from `published`, and the pair is the one place the difference is
     * written down. Both `'T` and `&T` refuse a trivial copy, so both set `published`; only the
     * first says *which* set the value is borrowed into, and only the first therefore has an origin
     * for a loan taken at it to inherit. See `grantedContract`, which is where it is spent.
     */
    U16 origin = kNoOriginSlot;

    /*
     * What the position says about the value's **interior** - `Maybe('v)`'s payload.
     *
     * The three fields above describe the root and this describes what is inside it, which is the
     * same division `Occurrence` makes between its contract and its shape. It is here rather than
     * carried beside the request for the reason the rest of the type exists: a request is read from
     * the destination and travels *down*, so a construction inside an `if` arm or a `match` arm is
     * told what it is filling by the same mechanism that tells a construction written directly.
     *
     * It cannot be inferred back up, which is what makes pushing it down the only answer. A field
     * filled from a bare name under an owning request is *copied* - `readMode` says so - and no
     * amount of looking at the value afterwards turns a copy into a borrow of the name it came from.
     *
     * Null for every position whose value owns what is inside it, which is all but a handful.
     */
    DeclPtr<OwnershipSchema> shape = nullptr;
};

// What a declared position - a parameter, a result, a record field - asks for. Its own contract,
// read once, so that a caller states which signature it selected and never what it wants.
inline Request requestOf(Contract contract) {
    /*
     * The three positions a trivial copy cannot satisfy, and the third is the **window**.
     *
     * The first two are `doc/spec/ownership.md`'s: a `&` parameter, "whose whole contract is writing
     * through to the same storage", and any external-origin qualifier, where "copying it would erase
     * the very relationship the signature published".
     *
     * A window is the same sentence about a container. `xs: [T]` describes storage the *caller*
     * keeps, so a copy of the container would hand the callee a window into the callee's own frame -
     * a different object with the same contents today, and nothing at all once the frame is reused.
     * The unique form is already covered by the `&` clause; the shared one is not, and it is the one
     * that matters most, because a `[U8 *4096]` is exactly the argument nobody wants copied.
     */
    auto published = contract.origin() == Origin::Slot ||
                     (contract.access() == Access::Unique && contract.origin() != Origin::Here) ||
                     contract.form() == AccessForm::SequenceWindow;

    return Request { contract.access(), published, contract.originSlot() };
}

/*
 * The contract a loan of this storage holds **at the position it was taken for** - the whole of what
 * `Builder::addRead` records about a borrow.
 *
 * `reborrowedContract` is the storage's half and answers almost every read. What it cannot answer is
 * a loan of storage this frame owns that is being handed to a position which *published* one:
 *
 *     data Pair {a: 'Int, b: 'Int}
 *     let &x = 7 :: Int
 *     let p = Pair {a: x, b: y}
 *
 * The field holds a reference to `x` - writing `x = 9` afterwards is observable through `p.a`, which
 * is the whole reason a field is declared `'Int` rather than `Int`. The storage half alone answers
 * `CallBounded`, and the manifestation of a call-bounded shared loan of a trivially copyable scalar
 * is **the scalar** rather than its address: the register value went into an eight-byte field, and
 * reading the field back dereferenced the number 7.
 *
 * So the origin is the storage's *or the position's*, whichever named one. That is not a claim about
 * extent: which origin sets a value is actually in is what the loan analysis computes from the
 * places, and `checkReturnConformance` reads the *parameter's* declared contract through the root
 * place rather than through any node's. What the field decides here is representation, which is what
 * `reborrowedContract` says it is for. The slot number travels with it on the terms that comment
 * already states for one: it is not a scope-local name and nothing reads it as one.
 *
 * **Only where the position named an origin**, which is `Request::origin` and not `published`. A
 * `&T` position refuses a trivial copy without naming a set, and a loan taken for one needs nothing
 * from this: exclusive access is an address already, whatever the origin says. Propagating an origin
 * there would put a frame-bounded loan in slot zero of a signature that never wrote a `'`.
 *
 * An **unpublished** position is unchanged, and that is what keeps a plain `v: Int` parameter a
 * register: `readMode` answers `CopyTrivial` there, so no loan of a trivial scalar is made at all.
 */
inline Contract grantedContract(Contract storage, Access access, Request wanted) {
    auto taken = reborrowedContract(storage, access);
    if(taken.origin() == Origin::Slot || wanted.origin == kNoOriginSlot) return taken;

    return slotContract(access, wanted.origin, taken.form());
}

/*
 * What a `let` asks - `doc/spec/ownership.md`'s two axes of a binding.
 *
 * `let x` asks for shared access, `let &x` for exclusive access and `let ->x` for ownership; none of
 * them publishes anything, so a `TrivialCopy` initializer is copied at all three and the second name
 * is a second value. What each one *becomes* depends on whether the initializer produced storage or
 * named it, and that is `readMode`'s answer rather than this one.
 */
inline Request bindingRequest(Access access) { return Request { access, false }; }

/*
 * What a position that only computes with a value asks - a condition, an operand of `+`, an
 * expression whose result is discarded.
 *
 * Shared and unpublished, which is the same pair a plain `let` asks with and is deliberately not the
 * same *statement*: an operand of an addition is not a binding, and the day arithmetic is a class
 * whose subject may be borrowed differently is the day these two stop agreeing. Naming both is what
 * makes that one edit rather than a search for which `Access::Shared` meant which.
 */
inline Request inspectRequest() { return Request { Access::Shared, false }; }

/*
 * One step of a schema slot's path - Analysis-Lens.md §12.2.
 *
 * A field index, or the object a pointer names. Packed into the `U16` the path already held rather
 * than widened, because a path is interned per occurrence and there are two kinds of step: one bit
 * discriminates and fourteen index a field, which is more than `Place::Field` itself can name. Which
 * bit, and why not the obvious one, is at `kPointee`.
 *
 * A **named kind** rather than a reserved index, and that is the whole reason this exists as a type.
 * `kFunEnvironmentComponent` is already a magic index - the pair's second word, filed under `Field`
 * because a `Fun` has no fields - and a second magic number in the same list is the point at which
 * nobody can read a path. So a step that is not a field says so.
 *
 * `Pointee` is what makes `%source'a` sayable: a pointer has no field index for the storage it
 * names, and Analysis-Lens.md §11.5's requirement is exactly that the interior position be
 * describable. It is deliberately not a pointer feature - a yielded record holding a view of a
 * lens's own buffer needs the same position with a field in front of it.
 */
struct PathStep {
    /*
     * Bit 14, and **not** bit 15, which the container owns.
     *
     * `DeclList<U16>` is a `SmallList` whose embedded form packs its first few elements into the
     * space the pointer would occupy and marks each occupied slot with the element's *top* bit -
     * `embedded[count] = T(value | mask)`, masked off again by `contents`. So a path step that set
     * bit 15 was stripped on the way back out and read as a field index of zero: the qualifier was
     * accepted, interned, printed as `.0`, and meant nothing.
     *
     * Fourteen bits is every field index there is - a record with sixteen thousand fields is not a
     * program - and the alternative, widening the element, costs every schema in the program four
     * bytes a step to buy nothing.
     */
    static constexpr U16 kPointee = 0x4000;

    // A field index that reached the discriminant would read back as a pointee, so the bound is
    // checked where the step is made rather than left to the reader that finds a `*` it cannot
    // explain. An assert rather than a diagnostic because nothing bounds a record's field count
    // yet: when something does, it is that check this one stops being reachable from.
    static U16 field(U16 index) { assertTrue(index < kPointee); return index; }
    static U16 pointee() { return kPointee; }

    static bool isPointee(U16 step) { return (step & kPointee) != 0; }
    static U16 fieldIndex(U16 step) { return U16(step & ~kPointee); }
};

/*
 * One externally owned position inside a value's logical structure - Analysis-IR.md §5.11.
 *
 * The contract above qualifies the *root*; this qualifies a position *inside* the value, and the
 * two are not interchangeable. `a'Maybe(T)` is a shared occurrence of `Maybe(T)` through slot `a`
 * with no interior slots; `Maybe(a'T)` is an owned occurrence of `Maybe(T)` with one interior slot
 * at the payload. Without the second half, the two are one `Contract` beside one `Type` and the only
 * way to tell them apart is to put ownership back into the logical type - which is the mistake
 * `Type` exists to prevent.
 */
struct ExternalSlot {
    /*
     * The projection path from the value's root to the occurrence, as `PathStep`s.
     *
     * Never empty: the root's own qualification is the `Contract`, and a schema slot describes what
     * is *inside*.
     */
    DeclList<U16> path;

    /*
     * What this interior position may do with the value, and whose the value is.
     *
     * A whole `Contract` rather than the `{U16 origin; Access capability;}` pair it replaces, and
     * the reason is `Origin`. A slot could previously only be in a published set, so `a'Maybe(T)`
     * and `Maybe(a'T)` were both expressible and *a value handed over out of a frame* was not: the
     * position had no way to say `CallBounded`, which is Analysis-Lens.md §11.5's whole requirement
     * read in this model. Saying it needs no new enumerator, because the root has been saying it
     * since milestone 3 - it needs the interior position to be described by the same type the root
     * is. `access()` and `slot()` are what the pair used to hold; `form()` is the root's answer for
     * this position and is `WholeValue` until a window can be nested in one.
     */
    Contract contract;
};

/*
 * Whether two interior slots describe the same position under the same terms.
 *
 * A free function taking the base rather than an `operator ==`, because the path is a region list and
 * comparing it needs one. That is not a detail: an equality that compared the contract and quietly
 * skipped the path would make `Pair(a'T, U)` and `Pair(T, a'U)` the same interned shape,
 * which is the exact confusion the shape exists to prevent - and an `operator ==` with no base to
 * hand is how it comes to skip it.
 */
inline bool sameSlot(DeclBase base, ExternalSlot& a, ExternalSlot& b) {
    if(a.contract != b.contract) return false;
    if(a.path.size() != b.path.size()) return false;

    for(Size i = 0; i < a.path.size(); i++) {
        if(a.path.get(base, i) != b.path.get(base, i)) return false;
    }

    return true;
}

/*
 * The finite list of interior slots for one qualified occurrence - the *ownership shape*.
 *
 * Interned, so shape identity is handle identity and two occurrences with the same interior
 * structure are the same handle. That is what makes the shape usable as part of a manifestation key
 * and as the thing a call's argument compatibility compares: a structural relation that had to be
 * walked at every comparison would be walked differently by each pass that needed it.
 *
 * The empty shape is the **null handle**, so the overwhelmingly common case - a type with nothing
 * borrowed inside it - costs nothing and needs no allocation.
 */
struct OwnershipSchema {
    DeclList<DeclPtr<ExternalSlot>> slots;

    // The interned identity, which is what a hash of an occurrence uses. Assigned by `Shapes`.
    U32 id = 0;
};

/*
 * The interned shape table, one per compilation.
 *
 * Held by value on the program rather than allocated in the region it allocates into, for the reason
 * `Types` is: it needs a hash map, and a hash map is not something a bump allocator can hold.
 *
 * Two things intern a non-empty shape, and they are not the same statement. A record's schema is a
 * property of the *declaration* - `own.cpp` folds one per record with an external field and hands it
 * to `RecordType::schema` - while an `Occurrence::shape` is what a type application produces, either
 * by renumbering that schema against a loan argument (`Reader(a')`) or by naming a position the
 * declaration does not have (`Maybe('v)`). One table serves both, because the two describe the same
 * thing - a list of interior positions somebody else owns - and interning them apart would give one
 * structure two identities.
 */
struct Shapes {
    explicit Shapes(DeclArena& arena): arena(arena) {}

    DeclArena& arena;

    // The shape with these interior slots. Null for none, which is the empty shape's handle.
    DeclPtr<OwnershipSchema> intern(ExternalSlot* slots, Size count);

    /*
     * The shape a **capturing function value** has - one interior position, borrowed for the call.
     *
     * A closure is an owned pair whose second word names storage somebody else keeps, so it is the
     * ordinary "owned root, external interior" statement every `Maybe(a'T)` makes and needs no new
     * vocabulary. What is unusual is only where the position is: a `Fun` has no fields, so the path
     * names the pair's second word - the one component a function value has that is not code.
     *
     * The same shape for every closure whatever it captured, which is the point of it being a
     * *shape*: what it says is that there is one position inside and that the position is a loan of
     * this frame. Which loans it is actually rooted in is the origin analysis' answer, and a shape
     * that varied per lambda would be an identity where a structure belongs.
     *
     * `Origin::CallBounded` is what makes every escape check work with nothing added to it:
     * `originSlotAt` answers `kNoOriginSlot` for the position, so a result that carries it is a
     * result whose type has no origin to say so - which is `Analysis::checkReturnConformance`'s
     * existing message and its existing rule.
     */
    DeclPtr<OwnershipSchema> closure();

    /*
     * The shape of what is at one step in, with that step removed - `Maybe(Boxed('v))`'s payload.
     *
     * A qualified type argument nests, so a shape describes positions at every depth and the walk
     * that installs a component has to hand that component *its* share. The slots whose path starts
     * with `step` are what is inside it; the one whose path is exactly `[step]` is the component's
     * own root and is `fieldContract`'s answer rather than this one's, so it is left out here.
     *
     * Interned like any other shape, which is what keeps a component's shape comparable with the
     * shape a signature wrote for the same structure.
     */
    DeclPtr<OwnershipSchema> under(DeclPtr<OwnershipSchema> shape, U16 step);

    static bool isEmpty(DeclPtr<OwnershipSchema> shape) { return shape == nullptr; }

private:
    HashMap<U64, DeclPtr<OwnershipSchema>> shapes;
    DeclPtr<OwnershipSchema> closureShape = nullptr;
    U32 nextId = 1;
};

// Which component of a function value the closure shape's interior position names - the pair's
// environment word. A `Fun` has no fields, so this is the one index anything projects it by, and it
// is stated here rather than written as a literal at the two ends that agree about it.
static constexpr U16 kFunEnvironmentComponent = 0;

/*
 * A typed occurrence: what the value is, and what this occurrence may do with it.
 *
 * The three together, and not just the type: every place that records one - locals, block arguments,
 * node results, signature positions, record fields, SSA values - has an answer for all three and no
 * reason to infer any of them from the others.
 *
 *  - `type` is the logical type, and is what generics substitute and instance selection sees.
 *  - `contract` qualifies the root: owned or borrowed, from where, in which access form.
 *  - `shape` is the interned list of interior positions someone else owns. Null is empty.
 */
struct Occurrence {
    DeclPtr<Type> type = nullptr;
    Contract contract;
    DeclPtr<OwnershipSchema> shape = nullptr;

    bool operator == (const Occurrence& other) const {
        return type == other.type && contract == other.contract && shape == other.shape;
    }

    bool operator != (const Occurrence& other) const { return !(*this == other); }

    // Whether two occurrences manifest the same way. The whole triple, which is the point of the
    // triple: an owned `String` and a shared window into one agree on `type` and on nothing else.
    bool sameManifestation(const Occurrence& other) const { return *this == other; }
};

inline Occurrence owned(DeclPtr<Type> type) {
    return Occurrence { type, ownedContract(), nullptr };
}

inline Occurrence borrowed(DeclPtr<Type> type, Access access) {
    return Occurrence { type, borrowedContract(access), nullptr };
}

// The same value under a different root qualification, keeping the interior shape. What a read, a
// handover and a signature match all want, and the one place the pair is rebuilt.
inline Occurrence requalified(const Occurrence& occurrence, Contract contract) {
    return Occurrence { occurrence.type, contract, occurrence.shape };
}

/*
 * What a declared position asks for, whole - its contract *and* its interior shape.
 *
 * The overload every declaration site wants: a parameter, a result, a field and a handover each have
 * an `Occurrence`, and the two halves of it are two halves of one question. Taking the contract
 * alone was how a `Maybe('v)` result told its `return Just(g)` what to build and told the same
 * construction inside an `if` arm nothing at all.
 */
inline Request requestOf(const Occurrence& occurrence) {
    auto request = requestOf(occurrence.contract);
    request.shape = occurrence.shape;
    return request;
}

// How a position's contract is spelled where a *type* is printed - `(->Guard) -> I32`, and the
// position's own default as the second argument. One function, so a dump and a diagnostic spell one
// shape one way; the mark itself is argued where it is decided, in type.cpp.
StringView conventionMark(Contract contract, Access written);

StringView accessName(Access access);
StringView originName(Origin origin);
StringView accessFormName(AccessForm form);

} // namespace mir
