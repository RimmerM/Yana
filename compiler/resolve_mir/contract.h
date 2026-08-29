#pragma once

#include "type.h"

namespace mir {

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

    // Which origin set, when `origin` is `Slot`. Numbered within one ownership schema scope: the
    // enclosing function's signature or the enclosing record's fields. Zero is the anonymous `'`.
    U16 slot() const { return U16((packed >> kSlotShift) & kSlotMask); }

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
inline Contract borrowedContract(Access access) {
    return Contract { access, Origin::CallBounded, AccessForm::WholeValue, 0 };
}

// A loan through a published origin set - the `'` and `name'` spellings. The slot is numbered
// within the enclosing ownership schema scope; see `Contract::slot`.
inline Contract slotContract(Access access, U16 slot, AccessForm form = AccessForm::WholeValue) {
    return Contract { access, Origin::Slot, form, slot };
}

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
    // The projection path from the value's root to the occurrence, as field indices. Never empty:
    // the root's own qualification is the `Contract`, and a schema slot describes what is *inside*.
    DeclList<U16> path;

    // Which published origin set is behind this position, numbered in the same scope as
    // `Contract::slot`.
    U16 origin = 0;

    Access capability = Access::Shared;
};

/*
 * Whether two interior slots describe the same position under the same terms.
 *
 * A free function taking the base rather than an `operator ==`, because the path is a region list and
 * comparing it needs one. That is not a detail: an equality that compared `origin` and `capability`
 * and quietly skipped the path would make `Pair(a'T, U)` and `Pair(T, a'U)` the same interned shape,
 * which is the exact confusion the shape exists to prevent - and an `operator ==` with no base to
 * hand is how it comes to skip it.
 */
inline bool sameSlot(DeclBase base, ExternalSlot& a, ExternalSlot& b) {
    if(a.origin != b.origin || a.capability != b.capability) return false;
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
 * Nothing in the language subset built so far has an interior slot, so every shape interned today is
 * the empty one and every handle is null. The table exists now because the alternative - adding
 * interior origins once loans exist - is how the current resolver ended up with provenance as a
 * relation parallel to liveness instead of a field of the thing it qualifies.
 */
struct Shapes {
    explicit Shapes(DeclArena& arena): arena(arena) {}

    DeclArena& arena;

    // The shape with these interior slots. Null for none, which is the empty shape's handle.
    DeclPtr<OwnershipSchema> intern(ExternalSlot* slots, Size count);

    static bool isEmpty(DeclPtr<OwnershipSchema> shape) { return shape == nullptr; }

private:
    HashMap<U64, DeclPtr<OwnershipSchema>> shapes;
    U32 nextId = 1;
};

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

StringView accessName(Access access);
StringView originName(Origin origin);
StringView accessFormName(AccessForm form);

} // namespace mir
