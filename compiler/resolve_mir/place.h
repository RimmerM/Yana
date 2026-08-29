#pragma once

#include "contract.h"

namespace mir {

struct Global;

/*
 * Where an object lives - Analysis-IR.md §10.4's placement lattice.
 *
 * A rung rather than a bool, and decided *here* rather than in SSA. Frame, region and heap placement
 * is what ownership analysis answers: whether a local escapes its frame, whether it outlives the
 * call that created it, and whether an ambient region witness is in scope are all questions about
 * the MIR the escape analysis runs over. SSA carries the answer down to the allocation instruction;
 * it does not get to make one.
 *
 * Three rungs from the start, and reachable rather than reserved. The current compiler's equivalent
 * has the region enumerator and never selects it, which left the *decision* shaped like a two-way
 * choice between frame and heap; stating three means the escape analysis that arrives later ranks
 * them instead of learning a third answer. Only `Frame` is produced at this milestone.
 */
enum class StorageClass: U8 {
    Frame,
    Heap,
    Region,
};

StringView storageClassName(StorageClass storage);

/*
 * A place: an object, or a projection of one - Analysis-IR.md §5.2.
 *
 * Interned per body behind a dense `PlaceId`, so an instruction stores four bytes rather than an
 * embedded root plus a projection list, and so that prefix and ancestry tests - which are what
 * borrow overlap is - are pointer walks rather than structural comparisons.
 *
 * A place is a parent link plus one step. Appending `.field` allocates one record and shares the
 * whole prefix, which is the property that makes the representation worth interning at all: a
 * function that touches `x.a.b` and `x.a.c` has four records and not six, and the two share the
 * ancestor that decides whether they conflict.
 *
 * Overlap is deliberately not decided here. `Field` steps of one parent may be disjoint, `Index`
 * steps of one parent are not unless a separately justified rule proves it, and `Deref` of a raw
 * address leaves the safe model entirely. That reasoning belongs to the loan checker, over these
 * records.
 */
struct Place {
    enum Kind: U8 {
        // Roots.
        Local,        // payload: the local's index in the body
        Global,       // payload: the global's declaration-region offset
        Loan,         // payload: the loan id in the body
        Address,      // payload: the node id producing a raw address

        // Projections. `parent` is set for exactly these.
        Field,        // payload: the field index within the parent
        Index,        // payload: the node id producing the index
        Deref,        // payload: unused

        KindCount,
    };

    Place(Kind kind, MirPtr<Place> parent, U32 payload, Occurrence occurrence, bool writable):
        parent(parent), occurrence(occurrence), payload(payload), kind(kind), writable(writable) {}

    // Null for a root, set for every projection. The one field the overlap test walks.
    MirPtr<Place> parent;

    /*
     * What is at this place, and what an access through it may do.
     *
     * Both, and not just the type: a field of a record the function only borrows is not owned here,
     * and a place is exactly where that has to be recorded - the alternative is re-deriving it from
     * the root's contract at every use, which is the walk this representation exists to delete.
     */
    Occurrence occurrence;

    /*
     * The step, in full.
     *
     * Thirty-two bits and not a truncation of them. A global's payload is a declaration-region
     * offset and a node payload is a node ID, and both reach past what a packed interning key had
     * room for - see `Builder::internPlace`, which is where two distinct places used to be able to
     * intern as one.
     */
    U32 payload;
    Kind kind;

    /*
     * Whether an assignment through this place is legal - `let &x` and `let &` on a global.
     *
     * Recorded here rather than re-derived from the root at every write, which is the same argument
     * `occurrence` makes one field up: a projection of a place nothing may write is a place nothing
     * may write, and a walk back to the root to find that out is a walk this representation exists
     * to delete. It says nothing about initialization - a `let x` place is written exactly once, by
     * the `Destination::Init` that establishes it.
     */
    bool writable = false;

    // Assigned by the interning table. The identity everything else stores.
    PlaceId id;

    bool isRoot() const { return kind <= Address; }

    /*
     * There is deliberately no `mayBeWritten()` combining the two fields above.
     *
     * They answer different questions and a write checks them at different times. `writable` is
     * about the *binding* - `let x` against `let &x` - and it does not forbid the one `Init` that
     * establishes the place, only every write after it; the access on `occurrence.contract` is about
     * the *storage*, and forbids a write through a shared borrow whatever the binding said. A helper
     * that conjoined them read plausibly and rejected every initialization of an immutable local,
     * which is what it did the first time it existed.
     */
};

StringView placeKindName(Place::Kind kind);

} // namespace mir
