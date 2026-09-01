#pragma once

#include "place.h"

namespace mir {

struct Body;

/*
 * One borrow, with a place and a capability - Analysis-IR.md §6.4.
 *
 * A loan exists because a *read* took one: `read borrow.shared (c)` is where storage becomes a value
 * somebody else still owns, and this is the record of what was borrowed and under what terms. Every
 * loan in a body is minted by `Builder::addRead`, so there is one place a borrow can come from and
 * no pass below can invent a second.
 *
 * Deliberately **not** an extent. Where a loan ends is a CFG answer - the last use of any value that
 * depends on it - and §2.2's split is that a builder may record what the source said and may not
 * decide what the graph implies. `analyze_loan.cpp` computes the extent from the origin sets, which
 * is the same fixpoint that answers what a value is rooted in.
 */
struct Loan {
    Loan(PlaceId place, Access capability, LocationId source):
        place(place), source(source), capability(capability) {}

    // Assigned by the builder; dense within one body, which is what lets a live-loan set be a byte
    // per loan rather than a hash set.
    LoanId id;

    // The storage borrowed. A place rather than a local, because `p.left` and `p.right` are two
    // loans that do not conflict and one that names only the record could not say so.
    PlaceId place;

    LocationId source = kNullLocation;

    // Shared or unique - never `Owned`. A loan is by definition an occurrence somebody else owns.
    Access capability = Access::Shared;
};

/*
 * Whether an access to `a` may reach the storage `b` names - Analysis-IR.md §5.2.
 *
 * The whole of overlap, and it is a walk over the interned parent chains rather than a structural
 * comparison, which is what interning places bought. Two places overlap when one is an ancestor of
 * the other; two `Field` steps of one parent with different indices are disjoint, which is what
 * makes `p.left` and `p.right` two loans that coexist; and an `Index` or a `Deref` step is the
 * conservative answer, because two indices into one container may be the same element and §5.2 does
 * not claim to prove otherwise.
 *
 * Two different roots are disjoint. That is sound only because a root is *storage this body names* -
 * a local, a global - and the one root that is not, a raw address, answers conservatively.
 */
bool placesOverlap(MirBase base, DeclBase decl, Body& body, PlaceId a, PlaceId b);

} // namespace mir
