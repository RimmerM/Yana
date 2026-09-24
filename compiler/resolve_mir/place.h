#pragma once

#include "type.h"

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
 * **About an object.** A run's slots are not one: there is no `Object` here for them, and the
 * frame that would keep them is not always the frame the request is written in - `String.ofCapacity`
 * and `growArray` ask for storage the *caller* keeps, so nothing can be decided until the inliner
 * has moved the request into that body. So `resolve_ssa/opt/place.cpp` decides that one, and it
 * decides it by answering `NodeRunSlots`' placement bit rather than by rewriting anything this file
 * placed. The shipping stage arrives at the same tier from the other end and re-runs its escape
 * analysis behind `inlineCalls`.
 *
 * Three rungs from the start, and reachable rather than reserved. The current compiler's equivalent
 * has the region enumerator and never selects it, which left the *decision* shaped like a two-way
 * choice between frame and heap; stating three means the escape analysis that arrives later ranks
 * them instead of learning a third answer.
 *
 * `Frame` and `Heap` are both produced - `selectStorage` below is the one thing that says which, and
 * `analyze_escape.cpp` is where it is called from. `Region` is still unreachable, and `verifyBody`
 * refuses it: what it means is "in the region the ambient witness names", and there is no witness
 * slot in a generic environment for one to be named in.
 */
enum class StorageClass: U8 {
    Frame,
    Heap,
    Region,
};

StringView storageClassName(StorageClass storage);

/*
 * What was established about one object, as the inputs the rung is ranked from - Analysis-IR.md
 * §10.4, Implementation-Regions.md §4.
 *
 * A record of *findings* rather than a decision, and `selectStorage` is the only thing that turns it
 * into one. That split is the whole of decision D6: the escape analysis, the `@heap` attribute and
 * the region witness each establish one field here, and none of them gets to say where the object
 * goes - so adding regions is a case in one function rather than a second placement rule somewhere
 * that has to agree with the first.
 */
struct Placement {
    /*
     * `@heap` on the binding - `doc/Design.md`'s "forces that one binding onto the general heap".
     *
     * One-directional by construction: it can move an object off the frame and never onto it,
     * because the frame answer is the one the analysis gives when it has *proved* the frame is
     * enough, and an attribute that could overrule that would be an attribute that can dangle.
     */
    bool demanded = false;

    /*
     * Whether the storage is reachable after this frame returns - the escape analysis' answer.
     *
     * About the **storage** and not the value: a value returned by copy leaves the frame and its
     * slot does not, which is why nearly nothing in a value-semantics IR sets this. See
     * `analyze_escape.cpp`, which lists what does.
     */
    bool outlivesFrame = false;

    /*
     * Whether an ambient region witness is in scope to place this in - Implementation-Regions.md §2.
     *
     * Always false, and it is a field rather than a `false` written into the ranking so that the day
     * a witness exists is a day one producer changes. §4's ladder is stack, then region, then heap,
     * and the ranking below is that sentence.
     */
    bool regionWitness = false;
};

/*
 * The rung - §10.4's lattice, ranked cheapest first.
 *
 * **The one function that decides where an object lives.** Everything else establishes a field of
 * `Placement` and calls this; nothing else writes an `Object::storage`. That is what makes the
 * region rung a rule rather than a mechanism: the line marked below is the whole of what
 * Implementation-Regions.md §4 adds to this stage's placement, and it is one line because the
 * findings it ranks are already separate from the ranking.
 */
StorageClass selectStorage(const Placement& asked);

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
 * `Discriminant` is which constructor a value of a sum currently is - Analysis-IR.md's G7d. A
 * projection rather than a node, because it is *storage the value has*: it is read exactly as a
 * field is, it is covered by a borrow of the record exactly as a field is, and a value reached
 * through one is an ordinary integer. What it is not is something a program can write - the only
 * thing that projects one is a pattern, which is what makes reading it a proof rather than a guess.
 *
 * `Property` is `Field` for a subject the body cannot see the layout of - `a.field: b`. It names the
 * *requirement* rather than an index, because the body is resolved once and the index is what
 * differs per instantiation; where it becomes an index is the conversion, which knows the
 * substitution. Everything else about it is a field: it projects, it names one component of its
 * parent, and two of them with different payloads are two objects.
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
        Property,     // payload: the property requirement's index in the enclosing context
        Discriminant, // payload: unused
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

/*
 * The root a place projects from - the walk `boxesVisibleAt` below and `ssa::convert.cpp`'s
 * address-relocation test both do, one instruction at a time up the `parent` chain, to ask two
 * different questions about the same fact. Every projection has a `parent` and every root does not,
 * so the loop ends exactly where `isRoot()` above would say yes.
 */
inline Place& rootPlace(MirBase base, Place& place) {
    auto root = &place;
    while(root->parent) root = base[root->parent];
    return *root;
}

/*
 * The step nearest the root at which a place leaves **storage its root owns** - a projection whose
 * own occurrence is a loan, or a `Deref` through one - and null where every step is the root's own.
 *
 * The one walk for the question "who owns the storage this place names", which a record update asks
 * (is the loan field the copy's own slot?) and `Resolver::unwrittenSource` asks (can anything but
 * the root's binding write these bytes?). Nearest the root and not nearest the place, because below
 * the first borrowed step every step is somebody else's storage whatever its own occurrence says,
 * and the first one is what both callers compare against.
 *
 * Not the writability walk `Resolver::loanFieldWriteRefused` makes, deliberately: an exclusive loan
 * field is not the root's storage and may still be written, so "where did writability go" and "whose
 * storage is it" stop at different steps under `box.exclusive.shared.value`.
 */
inline Place* firstBorrowedStep(MirBase base, Place& place) {
    Place* found = nullptr;
    for(auto at = &place; at->parent; at = base[at->parent]) {
        if(at->kind == Place::Deref || !at->occurrence.contract.isOwned()) found = at;
    }
    return found;
}

/*
 * Whether a teardown at this place can see where the value's interior boxes came from -
 * `ssa::InstDrop::boxesVisible`, and `ssa::BoxRelease` for what it decides.
 *
 * The **root** of the place answers it, and nothing else does. Storage this frame owns had its boxes
 * allocated by whatever built the value, and that is code in this program with this program's
 * allocator; storage reached through a borrow, a global or a raw address belongs to somebody whose
 * placement decision is not visible here.
 *
 * A **local** is the case that goes both ways: an owned local is this frame's, and a borrowed
 * parameter is a local whose contract says the storage is the caller's. `Contract::isOwned` is the
 * same question `buildPaths` asks to decide whether a local gets a move path at all.
 *
 * Here rather than in the conversion because two callers ask it: the conversion, which puts the
 * answer on the drop it emits, and `settleBoxProvenance`, which asks it of the whole program to
 * decide whether the answer is ever anything but `Owned`.
 */
bool boxesVisibleAt(MirBase base, Place& place);

} // namespace mir
