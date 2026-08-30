#pragma once

#include "inst.h"
#include "constant.h"
#include "loan.h"
#include "analyze.h"

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
     * Where the object this local names lives - Analysis-IR.md §10.4's placement lattice.
     *
     * Here rather than only on the SSA allocation, because frame, region and heap placement is what
     * *ownership analysis* decides: whether a local escapes, whether it outlives the call that made
     * it, and whether an ambient region witness is in scope are all questions about the MIR the
     * escape analysis runs over. SSA carries the answer down; it does not make it.
     *
     * Only `Frame` is produced at this milestone, and `Region` is what an ambient region witness
     * will select. Three rungs from the start rather than two and a later surprise: the current
     * compiler's equivalent reserves the region enumerator and never selects it, which left the
     * decision shaped like a two-way choice.
     */
    StorageClass storage = StorageClass::Frame;
};

/*
 * A basic block, with arguments - §5.9.
 *
 * Arguments rather than phis, so that the transfer of an owned value happens on the predecessor's
 * terminator (see `Edge`). A block's parameter list is the only thing a successor states about a
 * merge; which arm moved and which borrowed is stated by the arm.
 */
struct Block {
    Block(BlockId index, StringId name): name(name), index(index) {}

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
     * A dense table for the same reason the node table is one: a live-loan set is then a byte per
     * loan rather than a set of handles, and the overlap check is a walk over two arrays. Minted by
     * `Builder::addRead` and by nothing else.
     */
    RegionArray<MirRegion, MirPtr<Loan>> loans;

    /*
     * The constants this body names, by value, behind a dense index.
     *
     * In the *MIR* region: a literal in a function body is body data and has no business outliving
     * the body. Storing the sixteen bytes rather than a handle to a declaration-region node is what
     * makes that possible at all - see `Constant`, where the lifetime argument is written out. The
     * dense index is what keeps a packed operand's 26 bits enough to name one.
     */
    RegionArray<MirRegion, Constant> constants;

    bool sealed = false;

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
    Loan* loan(MirBase base, LoanId id) const { return base[loans.get(base, id.index)]; }
};

} // namespace mir
