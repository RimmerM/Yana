#pragma once

#include "inst.h"
#include "constant.h"
#include "loan.h"
#include "analyze.h"
#include "gen.h"

namespace mir {

struct Function;

/*
 * A named storage slot in a function.
 *
 * `occurrence` carries the contract, which is what the current resolver splits across a `convention`
 * on the local and an owner discovered by a side analysis. One field: a local declared `&` has
 * `Access::Unique` and `Origin::CallBounded`, and nothing has to pair it with a producing
 * instruction to find that out.
 */
struct Local {
    Local(StringId name, Occurrence occurrence, LocationId source, bool writable):
        occurrence(occurrence), name(name), source(source), writable(writable) {}

    Occurrence occurrence;
    StringId name {};
    LocationId source = kNullLocation;

    // Index in `Body::locals`, which is what a `Place::Local` root names.
    LocalId index;

    /*
     * The continuation region this storage was declared in - `Continuation`.
     *
     * Which scope of the body owns it, which the CFG cannot say. Read by the conversion: a local
     * declared inside a `Handover` is the continuation's own and goes with it when the region is
     * outlined, and one declared outside becomes a capture of the environment instead.
     *
     * It deliberately says nothing about *when* the storage dies. That is the ordinary rule - "an
     * object is released at the first point at which it is owned and no longer live" - and a second
     * rule here for one kind of scope is what the two lowerings would then disagree about; where a
     * hand-over really has to outlive the call, the loan is what says so.
     */
    ContinuationId region;

    /*
     * Which of the function's parameters this local *is*, or `kNotAParameter`.
     *
     * A parameter is storage like any other local - it has a place, it can be projected, it can be
     * borrowed - and the one thing that is different about it is where its contents came from: the
     * call established them, so there is no `write.init` in the body that establishes it and the
     * initialization dataflow has to start it as owned rather than as uninitialized. That is one
     * fact, so it is one field, and it is here rather than as a separate parameter table because a
     * second table would be a second place a place's root could be looked up in.
     *
     * The alternative was to give the entry block parameters and copy them into locals, which is the
     * same storage described twice: the entry's parameter list would be the signature and the local
     * would be the place, and every pass that wanted "where does `x` live" would have to know both.
     */
    static constexpr U32 kNotAParameter = ~U32(0);
    U32 parameter = kNotAParameter;

    bool isParameter() const { return parameter != kNotAParameter; }

    /*
     * `let &x` - whether this local may be assigned after it is initialized.
     *
     * Separate from the contract, deliberately. `Access` is about an *occurrence's* relationship to
     * a value someone else may own, and an owned local is `Owned` whether or not it may be written;
     * folding mutability into it would make `let x` and `let &x` two contracts for what is one
     * ownership relationship, which is the conflation `Contract` exists to avoid. `Global` carries
     * the same flag beside its own occurrence for the same reason.
     */
    bool writable = false;

    /*
     * Whether this local is a **lambda's captured environment** - `resolve_lambda.cpp`.
     *
     * Read by one thing: the diagnostic for a closure that escapes its frame. What the ownership
     * analysis finds is a loan of this local, and what it would otherwise say is that the result
     * refers to storage whose name the author never wrote - so the flag is what turns a true message
     * about a synthesized local into a true message about the closure the author did write.
     */
    bool environment = false;

    /*
     * `->x` - whether this local exists only so that the disposal the source wrote has a place.
     *
     * The disposal is not a construct of its own below the resolver: it reads the value under the
     * ordinary owned request and writes it into a temporary, which is dead from the next position
     * on, so the drop the source asked for is the drop the ordinary placement already puts there.
     * `doc/spec/ownership.md` - "`->x` emits whatever the teardown is - authored, derived, or
     * nothing at all" - is that sentence, and desugaring is what makes it true for all three.
     *
     * What the flag carries is the one thing the desugaring loses: whether the *source* asked. A
     * value of an `@explicitDrop` type dying in a temporary is exactly the diagnostic the attribute
     * exists for, and the temporary a disposal makes is exactly the case that must not report - so
     * `analyze_drop.cpp` reads this and nothing else does.
     */
    bool disposal = false;

    /*
     * The lineage of whatever comes to rest here - `Object`, and **not** a placement.
     *
     * This field held a `StorageClass` and that was the defect: placement is a property of the
     * *object*, and a local is a name for storage that an object may travel into and out of. Two
     * locals a move connects hold one object between them, and asking each of them separately where
     * it lives is how the answer comes to be two answers - so the question is asked of the lineage,
     * through `storageOf`, and a local only says which lineage it is part of.
     *
     * Invalid for a local that never holds an owner - a borrowed binding names somebody else's
     * object and starts no lineage of its own.
     */
    ObjectId object;

    /*
     * `@heap` written on the binding - `doc/Design.md`'s per-binding placement override.
     *
     * On the **local** and not on the object, because that is where the author wrote it: a lineage is
     * a fact this stage derives and a binding is a thing a program says. `selectPlacements` is what
     * carries it to the object, joined over every local that names the lineage, so `let @heap a =
     * ...; let b = a` places the one object once.
     */
    bool demandsHeap = false;

    /*
     * The constant this binding was established with, or `Constant::None` - what makes
     * `let width = vectorBytes` a number where it is read.
     *
     * Recorded only for a local that is **not** writable, which is the whole of what makes it sound:
     * nothing may assign to one after its `Init`, so the value it holds is forever the one written
     * here and `readPlace` may answer with it instead of a load. The storage still exists and is
     * still written - a published position takes a loan of it and `addressOf` names it - so this is
     * a fact *beside* the place rather than a place that went away.
     *
     * It is the same rule `readPlace` applies to an immutable global one scope up, and it is here
     * for the same reason: `lib/Native/Memory.native.yana` gates a `Vec(U8, 32)` load on
     * `width > 32`, and behind a frame slot the comparison is opaque, the arm survives, and the x64
     * backend refuses the load outright below v3. Nothing promotes that slot on an unoptimized
     * build, so the fold has to be here rather than in a pass.
     */
    Constant initial;
};

/*
 * One object - a thing whose teardown is counted, and whose lifetime has a place to be decided.
 *
 * Analysis-IR.md §10.4's placement lattice hangs off this and off nothing else. What makes it an
 * identity rather than a field on the producer is the rule the identity exists to state:
 *
 *  - **moving preserves a lineage.** `let b = a` does not make a second object; it makes the one
 *    object reachable through a second name, and everything decided about it - where it lives,
 *    whether its address is stable, whether it escapes - is decided once for both names.
 *  - **copying starts a lineage.** A duplicate is a second object with its own answers.
 *
 * The representation is a union-find over the objects, which is what makes "preserves" a *merge*
 * rather than a relation somebody has to keep transitive: a write of a value into a place unites the
 * two lineages, so a local holds exactly one object however many moves reached it, and a pass asking
 * where something lives gets one answer instead of a set to join. That is the choice this exists to
 * make: a set-valued may-relation is recoverable from producer/local pairs, which is precisely the
 * reconstruction the identity is here to prevent.
 */
struct Object {
    Object(ObjectId id, DeclPtr<Type> type, LocationId source):
        id(id), parent(id), type(type), source(source) {}

    ObjectId id;

    // The union-find link; its own id at the root. `Body::objectRoot` is the only reader, and it
    // path-compresses, so a chain of moves costs one walk however long it got.
    ObjectId parent;

    DeclPtr<Type> type = nullptr;
    LocationId source = kNullLocation;

    /*
     * Where this object lives - §10.4, on the lineage rather than on a slot.
     *
     * Written by `selectPlacements` and by nothing else, out of `selectStorage`'s one ranking.
     * `Frame` is the default because it is the answer for an object nothing established anything
     * about, and because a body that reported before the placement pass ran must still print.
     */
    StorageClass storage = StorageClass::Frame;
};

/*
 * A region of blocks with a nesting rule - Analysis-IR.md §10.1's continuation regions, in MIR.
 *
 * `ssa::Continuation` is the same structure one tier down, and it was built first with two of its
 * facts stated and two deferred for want of a producer. This is the producer: a lens call is a
 * terminator that enters a **`Handover`** - the caller's own blocks, which are the callee's
 * continuation - and control comes back at the region's destination when they fall out.
 *
 * A diamond is a diamond whatever its arms mean, so the scope is not recoverable from the shape.
 * The builder knows and says so, which is what lets §11's two rules be checks rather than
 * inferences: a loan opened inside a region may not have an extent that leaves it (§12.1's fourth
 * item), and a lens called inside another's continuation nests rather than outliving it (§11.4).
 *
 * One of the two facts `ssa::Continuation` deferred is here, and the other is smaller than it
 * looked. The **result contract** is `handover`, which is what the region's entry receives. The
 * **yield/resume relation** turns out to be `entry` and `destination` already: control enters at the
 * first and comes back at the second, and for a region that hands over once there is nothing else to
 * say. What is still missing is the *multi*-resume form - `Y <- R`, where a yield evaluates to what
 * the consumer handed back and the region is re-entered where it left off - and that arrives with
 * `iter fn`, which is the waypoint after this one.
 *
 * There is no region for the *callee's* blocks and there was one: the flattening this replaced
 * copied a lens's body into its caller, so the caller's CFG held two frames' worth of blocks and
 * needed a kind to tell them apart. A lens is a function now, so its blocks are its own body's.
 */
struct Continuation {
    enum Kind: U8 {
        Body,
        Handover,
    };

    /*
     * How many times a region runs per entry to its parent - §10.1's third fact.
     *
     * Not derived from the kind, and the two come apart at the first lens whose continuation may be
     * skipped: a transparent lens runs its continuation exactly once and a skipping one at most
     * once, with nothing in "this is a continuation" to say which. Stated where the region is made,
     * by the only thing that knows.
     */
    enum Multiplicity: U8 {
        Once,
        AtMostOnce,

        /*
         * Any number of times, which is what an `iter fn`'s `for` body is - and the one value of
         * this field that changes what an analysis walks rather than only what it may assume.
         *
         * The caller's CFG cannot show the loop, because the loop is in the callee: the body falls
         * out and the *iterator* decides whether to call it again, and the callee is opaque. So the
         * back edge is this fact rather than an edge, and `Analysis::forEachSuccessor` is the one
         * place it is read - a block that falls out of a `Many` region has that region's entry as a
         * successor too, which is what makes a move in a loop body a move on the next iteration.
         */
        Many,
    };

    Continuation(ContinuationId id, Kind kind, Multiplicity multiplicity, ContinuationId parent):
        id(id), parent(parent), kind(kind), multiplicity(multiplicity) {}

    ContinuationId id;

    // The enclosing region; invalid for the body's own, which is the only one with no parent.
    ContinuationId parent;

    // The one block the parent may enter this region at - the first block made inside it. A region
    // is entered at one point by definition; that is what makes it a continuation rather than a
    // subgraph, so the point is recorded when it is made rather than recovered afterwards.
    BlockId entry;

    /*
     * The one block outside this region an exit may go to - where it rejoins its parent.
     *
     * Deliberately **optional** where `entry` is not, and this is the milestone the asymmetry was
     * reserved for: a `Handover` rejoins at the cleanup that follows the `yield`, and §10.1's other
     * three exits - `return`, `break`, `continue` - leave it for an ancestor instead. So the rule is
     * "an exit goes to this region, to `destination`, or to an ancestor", which holds a region to
     * what its builder said without making single-exit a property of regions in general.
     */
    BlockId destination;

    /*
     * Where the caller resumes when the callee *failed* - the `lens.try`'s second edge, and invalid
     * for every region opened by a call that cannot.
     *
     * On the region rather than found by walking back to the terminator, for the reason
     * `destination` is: the analyses reach a region and not the node that opened it, and what they
     * need from this is the same thing they need from `destination` - a block of the parent that
     * control can be at once this region is done with. See `Analysis::forEachSuccessor`, which is
     * where it earns its place: an `iter fn` that fails part-way through has run its consumer's body
     * some number of times first, and that path exists in the *callee* rather than in any edge here.
     */
    BlockId otherwise;

    /*
     * What the callee **declared** it hands over, for a `Handover`, and empty on every other kind.
     *
     * The result contract `ssa::Continuation` named as missing, and deliberately the declaration's
     * rather than the value's: what actually arrives is the entry parameter's occurrence, which is
     * rooted in the storage the flattened callee really borrowed, and reading it off the region
     * would be the same fact stored twice. The declared contract is the one an *opaque* boundary has
     * instead of the storage, so it is what a signature has to be checked against and what the ABI
     * will be built from.
     */
    Occurrence handover;

    Kind kind = Body;
    Multiplicity multiplicity = Once;
};

/*
 * A basic block, with arguments - §5.9.
 *
 * Arguments rather than phis, so that the transfer of an owned value happens on the predecessor's
 * terminator (see `Edge`). A block's parameter list is the only thing a successor states about a
 * merge; which arm moved and which borrowed is stated by the arm.
 */
struct Block {
    Block(BlockId index, StringId name, ContinuationId region):
        name(name), index(index), region(region) {}

    // The parameters, as `Argument` nodes - see inst.def. They have IDs in the body's node space and
    // occurrences of their own, so an operand naming one names it exactly as it names any other
    // value; what they are not is members of `nodes` below, since they hold on entry rather than
    // occupying a position among the instructions.
    MirList<MirPtr<Node>> arguments;

    // In order. Every entry but the last is a non-terminator; the last is the terminator once the
    // block is sealed, and `verifyBody` is what holds a body to that.
    MirList<MirPtr<Node>, false> nodes;

    StringId name {};
    BlockId index;

    // Which continuation region owns this block - `Continuation`. Set when the block is made and
    // never derived: the builder knows which scope it is emitting into, and afterwards nobody can.
    ContinuationId region;

    // Filled by `sealBody` from the edges, so that a backward analysis has predecessors without
    // walking the whole function. Derived, and therefore not something a builder may write.
    MirList<MirPtr<Block>> incoming;
};

/*
 * One function's MIR body.
 *
 * Everything here is in the MIR region and dies with it. The dense tables are what §8.6 asks for: an
 * analysis indexes a scratch array by `NodeId`, `LocalId`, `PlaceId` or `BlockId` rather than
 * hanging a field off every node that only one pass reads.
 *
 * `sealed` separates the two halves of §2. Before it, a builder is appending and the only checkable
 * invariants are per-node. After it, the CFG exists, `verifyBody` runs, and the passes that need the
 * finished graph - liveness, initialization, loan overlap, drop elaboration - may run.
 */
struct Body {
    explicit Body(MirPtr<Block> entry): entry(entry) {}

    MirPtr<Block> entry;

    RegionArray<MirRegion, MirPtr<Block>> blocks;
    RegionArray<MirRegion, MirPtr<Local>> locals;
    RegionArray<MirRegion, MirPtr<Node>> nodes;
    RegionArray<MirRegion, MirPtr<Place>> places;

    /*
     * Every borrow this body takes, in creation order - `Loan`.
     *
     * A dense table for the same reason the node table is one: a live-loan set is then a bit per
     * loan rather than a set of handles, and the overlap check is a walk over two arrays. Minted by
     * `Builder::addRead` and by nothing else.
     *
     * **By value**, unlike every other table here, and the difference is that a `Loan` is sixteen
     * fixed bytes with nothing to derive from. The other tables hold handles because their elements
     * are polymorphic or grow lists; this one held a handle to a separate sixteen-byte allocation,
     * which cost a pointer table beside the objects and made `loan()` two indirections - on the
     * lookup the extent fixpoint and the overlap walk make in their innermost loops.
     *
     * The condition that comes with it is the one `container.h` states for any list that is not
     * pointed into: nothing may hold a `Loan*` across a push. Nothing does - the one mint site fills
     * the entry it just pushed, and every analysis runs over a sealed body.
     */
    RegionArray<MirRegion, Loan> loans;

    /*
     * Every object this body creates, and which one each node produced - `Object`.
     *
     * `nodeObject` is dense over node IDs and is `ObjectId::kNone` for the nodes that produce no
     * object, which is most of them: a borrow names somebody else's, a scalar is not one, and only a
     * value with a lifetime this frame is answerable for gets an identity. A side table rather than
     * a field on `NodeResult` for §8.6's reason, and *on the body* rather than in an analysis
     * because the lineage is what the builder saw the source say - a `Take` is a move because the
     * source moved, not because a pass worked it out.
     */
    RegionArray<MirRegion, MirPtr<Object>> objects;
    RegionArray<MirRegion, U32> nodeObject;

    /*
     * The constants this body names, by value, behind a dense index.
     *
     * In the *MIR* region: a literal in a function body is body data and has no business outliving
     * the body. Storing the sixteen bytes rather than a handle to a declaration-region node is what
     * makes that possible at all - see `Constant`, where the lifetime argument is written out. The
     * dense index is what keeps a packed operand's 26 bits enough to name one.
     */
    RegionArray<MirRegion, Constant> constants;

    /*
     * The continuation regions, root first - `Continuation`.
     *
     * Never empty: a body has at least its own, which is what every block names when nothing nested
     * is open. Handles rather than values, on the same terms as the block table - a region is
     * pointed at while it is open and filled in twice, at its entry and at its destination.
     */
    RegionArray<MirRegion, MirPtr<Continuation>> regions;

    bool sealed = false;

    /*
     * The generic context this body was resolved in - `Function::gen`, and null for a plain one.
     *
     * Held here rather than reached through the function because a `Place` naming a **requirement**
     * has to be readable from the body alone: `Place::Property`'s payload is a position in
     * `GenContext::properties`, and the dump, the verifier and the conversion each hold a body and
     * a place with nothing between them. A handle into the declaration region, so it outlives the
     * MIR arena's release like every other declaration a body names.
     */
    DeclPtr<GenContext> gen = nullptr;

    /*
     * What the ownership analysis concluded - `analyze.h`, filled by `analyzeBody` once the body is
     * sealed and read by the SSA conversion, which emits the teardown.
     *
     * A side table rather than fields on the nodes (§8.6), and in the MIR region rather than the
     * declaration region: it is about one body and dies with it. Null until the pass has run, which
     * is what distinguishes "nothing to drop" from "not asked yet".
     */
    MirPtr<Elaboration> elaboration = nullptr;

    Node* node(MirBase base, NodeId id) const { return base[nodes.get(base, id.index)]; }
    Place* place(MirBase base, PlaceId id) const { return base[places.get(base, id.index)]; }
    Block* block(MirBase base, BlockId index) const { return base[blocks.get(base, index.index)]; }
    Local* local(MirBase base, LocalId index) const { return base[locals.get(base, index.index)]; }
    Constant constant(MirBase base, ConstantId id) const { return constants.get(base, id.index); }
    Loan* loan(MirBase base, LoanId id) const {
        assertTrue(id.index < loans.size());
        return loans.pointer(base) + id.index;
    }

    Continuation* region(MirBase base, ContinuationId id) const {
        return id.valid() ? base[regions.get(base, id.index)] : nullptr;
    }

    // Whether `outer` encloses `inner`, reflexively. The nesting test itself, so that the verifier,
    // the yield check and everything after them ask it once rather than each walking the chain.
    bool encloses(MirBase base, ContinuationId outer, ContinuationId inner) const {
        for(auto at = inner; at.valid(); at = region(base, at)->parent) {
            if(at == outer) return true;
        }

        return false;
    }

    Object* object(MirBase base, ObjectId id) const {
        return id.valid() ? base[objects.get(base, id.index)] : nullptr;
    }

    /*
     * The lineage an object belongs to, with the chain compressed on the way out.
     *
     * `const` and mutating, deliberately: what it writes is the path, and the answer is the same
     * before and after. A caller that had to remember to compress would be a caller that forgets.
     */
    ObjectId objectRoot(MirBase base, ObjectId id) const {
        if(!id.valid()) return id;

        auto root = id;
        while(object(base, root)->parent != root) root = object(base, root)->parent;

        while(object(base, id)->parent != root) {
            auto next = object(base, id)->parent;
            object(base, id)->parent = root;
            id = next;
        }

        return root;
    }

    // What one node produced, or an invalid id. Through this rather than off the array, so that a
    // node built before the table was extended answers "none" instead of reading past the end.
    ObjectId objectOf(MirBase base, NodeId node) const {
        if(node.index >= nodeObject.size()) return ObjectId();
        return ObjectId(nodeObject.get(base, node.index));
    }
};

/*
 * Where the object a local holds lives - §10.4, asked of the lineage.
 *
 * The one function that answers placement, and the reason `Local` no longer carries a
 * `StorageClass`: two locals a move connects are one object, and the SSA allocation for either of
 * them has to agree about where it is. A local that holds no owner answers `Frame`, which is what a
 * borrowed binding's slot is - a name, in this frame, for somebody else's storage.
 */
StorageClass storageOf(MirBase base, Body& body, Local& local);

} // namespace mir
