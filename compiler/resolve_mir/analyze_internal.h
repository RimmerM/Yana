#pragma once

#include "decl.h"

namespace mir {

/*
 * How much of one move path is still here - §5.10's lattice.
 *
 * `Uninitialized` and `Moved` are one state as far as a teardown is concerned and two as far as a
 * diagnostic is: "used before it was given a value" and "used after it was handed away" are
 * different mistakes with different fixes, and a lattice that collapsed them would report the wrong
 * one half the time.
 *
 * A parent whose child moved is `Moved` and its other children stay `Owned`, which is the whole of
 * partial ownership: reading the parent as a whole is then a use after a move, and releasing it is a
 * walk into the children that are still there.
 */
enum class PathState: U8 {
    Uninitialized,
    Owned,
    Moved,
    Maybe,
};

// The join at a merge. Two answers that agree are that answer; `Owned` meeting anything that is not
// gives `Maybe`, which is what a drop flag exists for; and the two "nothing here" states meet as
// `Moved`, the more informative of the two for the diagnostic that follows.
PathState meetState(PathState a, PathState b);

/*
 * One body's analysis, shared by the three files that make it up.
 *
 * The dense side tables are `Array`s held here rather than in the MIR region: they are scratch, they
 * are the size of blocks times paths, and the region is the one thing in this stage that cannot free.
 * `Elaboration` is what survives, and it holds only the conclusions.
 */
struct Analysis {
    Context& context;
    Program& program;
    Function& function;
    Body& body;
    MirBase base;
    DeclBase decl;
    Elaboration& out;

    U32 pathCount = 0;
    U32 blockCount = 0;
    U32 errors = 0;

    /*
     * Two maps from a place, and they answer different questions.
     *
     * `exact` is the path a place *is*, and is what a write kills and a move changes. `cover` is the
     * nearest tracked ancestor, and is what a read keeps alive. They differ for a place that is not
     * tracked itself but sits inside one that is - a trivially copied field of a record with a
     * teardown, an element of a container - and using one where the other belongs is how writing
     * `p.x` comes to look like a redefinition of `p` and lets `p.buffer` be released early.
     */
    Array<MovePathId> exact;
    Array<MovePathId> cover;

    // Per block, one byte per path: whether the path is live on entry to / on exit from the block.
    Array<U8> liveIn;
    Array<U8> liveOut;

    // Per block, one state per path, on entry and on exit. A byte each rather than a bit set: a body
    // has tens of paths and tens of blocks, and the packing would cost more to read than it saves.
    Array<PathState> stateIn;
    Array<PathState> stateOut;
    Array<U8> reached;

    /*
     * The loan tables - `analyze_loan.cpp`.
     *
     * `valueOrigins` is one byte per (node, loan) and is §5.11's origin environment: which loans the
     * value a node produces may be rooted in. `localOrigins` is the same question asked of storage,
     * and is the union over every write into it - deliberately flow-insensitive, because §2.2 says
     * an origin slot "is a set of possible origins" and does not claim the members have one lexical
     * lifetime, so a union is the answer rather than an approximation of one.
     *
     * `loanIn`/`loanOut` are the extents: one byte per (block, loan), from the backward fixpoint. A
     * loan is live where any value rooted in it still is, which is what makes the extent fall out of
     * the origin environment instead of being a second thing to keep in step.
     */
    U32 loanCount = 0;
    Array<U8> valueOrigins;
    Array<U8> localOrigins;
    Array<U8> loanIn;
    Array<U8> loanOut;

    U8* originsOfValue(NodeId node) { return valueOrigins.pointer() + node.index * loanCount; }
    U8* originsOfLocal(LocalId local) { return localOrigins.pointer() + local.index * loanCount; }
    U8* loansAt(Array<U8>& table, U32 blockIndex) { return table.pointer() + blockIndex * loanCount; }

    // The root of a projection chain - the local, global or address the place is reached through.
    Place* rootOf(PlaceId place);

    // The loans held by the storage a place is reached through, or null where it is storage this
    // frame owns. What a reborrow is allowed not to conflict with, and what a value read out of it
    // inherits.
    U8* originsOfPlace(PlaceId place);

    /*
     * The loans one *operand* may reach, whichever kind it is - the origin environment asked the
     * only way three passes all have to ask it.
     *
     * A value operand reaches what its value is rooted in and a place operand reaches what the
     * storage behind it is rooted in, and both are uses of the same lender: `idOf(d)` names a value
     * and `return d.n` names a place, and a rule that saw only the first let the loan behind `d` die
     * between them. One function rather than the pair inlined at each of the three call sites,
     * because they are three answers that have to agree - the origin environment builds it, liveness
     * keeps the lender alive by it, and the extent walk ends the loan by it.
     */
    U8* originsOfOperand(Operand operand);

    template<class... T>
    void report(LocationId source, StringView text, T&&... arguments) {
        errors++;
        context.diagnostics.error(text, source, forward<T>(arguments)...);
    }

    MovePath& path(MovePathId id) { return *out.path(base, id); }

    Block* block(U32 index) { return body.block(base, BlockId(index)); }
    U32 nodeCount(U32 index) { return block(index)->nodes.size(); }
    Node& nodeAt(U32 blockIndex, U32 position) {
        return *base[block(blockIndex)->nodes.get(base, position)];
    }

    // The paths, in an order where a parent always precedes its children - which is the order they
    // are created in, since a field is interned after the record it projects.
    U8* liveAt(Array<U8>& table, U32 blockIndex) { return table.pointer() + blockIndex * pathCount; }
    PathState* stateAt(Array<PathState>& table, U32 blockIndex) {
        return table.pointer() + blockIndex * pathCount;
    }

    /*
     * The three halves, in the order they run.
     */

    // analyze_path.cpp - the move-path tree, and the two place maps.
    void buildPaths();

    /*
     * analyze_loan.cpp - §6.4, in three steps that are one structure read three ways.
     *
     * `computeOrigins` is the forward fixpoint that answers what every value is rooted in;
     * `computeLoanExtents` is the backward one that turns that into a live range per loan; and
     * `checkLoans` walks the body once more with both in hand and reports the overlaps and the
     * signature conformance failures. They run *before* liveness, because a loan keeps the storage
     * it came out of alive and the move-path liveness has to read that - which is the one dependency
     * between the two analyses and the reason the order is what it is.
     */
    void computeOrigins();
    void computeLoanExtents();
    bool checkLoans();

    // One node, backwards, over the loan set. Shared by the extent fixpoint and by the check walk,
    // which needs the answer at every position rather than at every block - the same arrangement
    // `applyBackward` has for move paths, and for the same reason.
    void stepLoansBackward(U8* live, Node& node);

    void checkReturnConformance(Node& node);

    // The origin environment, into the `Elaboration` the dump and the conversion read. Copied rather
    // than computed there, because the scratch tables are `Array`s that die with the analysis and
    // §8.6 keeps the conclusions in the region and the working state out of it.
    void publishOrigins();

    // analyze_live.cpp - the backward fixpoint, and the `Forward` rewrite it licenses.
    void computeLiveness();
    bool resolveForwards();

    // The three steps of the backward transfer, shared with the drop walk, which needs liveness at
    // every position rather than at every block and recomputes it from each block's live-out.
    void markUse(U8* live, MovePathId id, bool exactly);
    void markKill(U8* live, MovePathId id);
    void useOfPlace(U8* live, PlaceId place);
    void applyBackward(U8* live, Node& node);

    // The name a diagnostic calls a path by: the root local's, which is what the author wrote.
    String pathName(MovePathId id);

    /*
     * analyze_drop.cpp - the forward fixpoint, its diagnostics, and where each teardown lands.
     *
     * `walkBlock` is one function read three ways - see `WalkMode` in that file. The mode is an
     * `int` here only so that the enumeration can stay private to the file that means it; every
     * caller is in there.
     */
    void computeInitialization();
    void placeDrops();
    void walkBlock(U32 index, PathState* state, int mode);
};

/*
 * Whether a value of this type is tracked at all.
 *
 * A type that releases nothing and duplicates for free has no move to record and no teardown to
 * place, so a path for one would be a row in every table for a fact nothing reads. Both halves are
 * asked because they are different reasons: a teardown means the end of the value's life is
 * observable, and a refusal to duplicate means a read of it is a move.
 */
bool isTracked(DeclBase base, DeclPtr<Type> type);

} // namespace mir
