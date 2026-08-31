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

struct OriginCursor;

/*
 * The field steps from a local down to a place, and whether the walk left the local's own storage.
 *
 * `fields` is the leading run of `Place::Field` payloads, in the order a schema slot's path is
 * written in; `opaque` says an `Index` or a `Deref` appeared, which is where a projection stops
 * being comparable with a path. A `Deref` here is not exotic and is not a failure: an external field
 * is interned as the slot's `Field` followed by a `Deref` of it, so `r.held.id` is `[0]` and opaque -
 * *through* the slot, and then inside an object this frame does not own.
 */
struct Projection {
    SmallArray<U16, 8> fields;
    bool opaque = false;
};

/*
 * One origin position of a local, and the position of the value at a place that it becomes.
 *
 * Both halves, because a projection renumbers: a whole read of `r` moves slot `i` to slot `i`, and a
 * read of `r.held` moves slot `held` to the *root* of the value that comes out, because what comes
 * out is the object the slot named rather than a value containing it.
 */
struct PositionMap {
    U16 from;
    U16 to;
};

/*
 * One body's analysis, shared by the three files that make it up.
 *
 * The dense side tables are held here rather than in the MIR region: they are scratch, they are the
 * size of blocks times paths or nodes times loans, and the region is the one thing in this stage
 * that cannot free. `Elaboration` is what survives, and it holds only the conclusions.
 *
 * Everything that is a *set* of blocks, paths or loans is bits - `BitMatrix` and `IndexSet`, and
 * `util/README.md` says which is for what. Everything that is a fixpoint is a worklist over
 * `order`. Both are one decision made in four places on purpose: these are four readings of one
 * graph, and four walks that had drifted into four shapes would be four things to keep in step.
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

    // Per block, one bit per path: whether the path is live on entry to / on exit from the block.
    BitMatrix liveIn;
    BitMatrix liveOut;

    /*
     * The storage a flattened lens's own frame owns - `Local::region`, and empty in every body that
     * calls no lens.
     *
     * One entry per owned local whose region is a `Frame`, holding that region and the path the
     * local is. `keepFrameLocals` reads it, and what it exists to say is that this storage belongs
     * to a callee rather than to the body it has been flattened into: it dies when the frame does,
     * on every way out of it, rather than at its own last use somewhere in the middle of the
     * caller's continuation.
     */
    struct FrameLocal {
        ContinuationId region;
        MovePathId path;
    };

    Array<FrameLocal> frameLocals;

    // Per block, one state per path, on entry and on exit. A byte each rather than a packed field:
    // the lattice has four values, so a bit table would be two bits and a shift on the walk that
    // reads it most, and this is the smallest of the tables here rather than the largest.
    Array<PathState> stateIn;
    Array<PathState> stateOut;
    IndexSet reached;

    /*
     * The loan tables - `analyze_loan.cpp`.
     *
     * `valueOrigins` is §5.11's origin environment: which loans each *position* of the value a node
     * produces may be rooted in. A position is the value's root or one of its interior schema slots
     * - see `originPositions` - so a record's two external fields are two sets and not one.
     *
     * The *local origin state* is the same question asked of *storage*, and it is asked **per
     * program point**: one bit per (point, position of an origin-carrying local, loan), holding what
     * each position is rooted in on entry to that point. §5.11's model is
     * `program point x slot -> origin set` and this is it read literally, both axes. It is walked
     * rather than tabulated - see `log`.
     *
     * It used to be one permanent union per local, over every write anywhere in the body, and that
     * is not a conservative reading of §2.2's "a slot is a set of possible origins" - it is a
     * different statement. §2.2 is about the origins one *value* may have; a local is storage that
     * holds a succession of values, and a union over all of them makes a reader that has been
     * refilled from a second stream go on referring to the first. That rejected
     *
     *     let &r = Reader {held: g, at: 0}
     *     r = Reader {held: h, at: 1}
     *     g.id = 9
     *
     * at the write to `g`, because `r` still carried the loan its first value had - which is the
     * shape every mutable stream or iterator adaptor is written in.
     *
     * The slot axis used to be collapsed the same way, and the same argument applies to it: a
     * record's origins were one set for the whole value, so reading one external field kept the
     * other's loan live and `Two {left: g, right: h}` made a write to `g` a conflict with a read of
     * `t.right`. That is a false rejection rather than a rule, and `separateLenders` in
     * `test/resolve/loans/Schemas.yana` is where it is now accepted.
     *
     * `localIn`/`localOut` are the rows per block, which is what the forward fixpoint iterates on.
     * What the walks after it read is an `OriginCursor` over those rows and the undo log below -
     * see `OriginCursor` for why the per-point answer is a walk rather than a table.
     *
     * `loanIn`/`loanOut` are the extents: one bit per (block, loan), from the backward fixpoint. A
     * loan is live where any value rooted in it still is, which is what makes the extent fall out of
     * the origin environment instead of being a second thing to keep in step.
     */
    U32 loanCount = 0;
    U32 loanWords = 0;
    BitMatrix valueOrigins;

    /*
     * The locals that can be rooted in a loan at all, densely renumbered.
     *
     * The state is locals times loans, so the locals in it are the ones the question means: a body's
     * `I32` counters carry no origin, and a row each for them would be width every walk pays for a
     * fact nothing reads.
     */
    /*
     * Where each value's and each local's origin positions start - the slot axis, as two prefix
     * sums.
     *
     * `valueRow` has one entry per node plus a sentinel, so the positions of node `n` are the rows
     * `valueRow[n]` up to `valueRow[n + 1]` and a value that carries no origins at all is the empty
     * range rather than a reserved index. `localRow` says the same of the local origin state, whose
     * rows are `originRowCount` in total.
     *
     * A prefix sum rather than a base and a count, because the two would be two tables to keep in
     * step and the sentinel makes the count a subtraction. The locals in it are still only the ones
     * the question means - a body's `I32` counters contribute no positions, and a row each for them
     * would be width every walk pays for a fact nothing reads.
     */
    Array<U32> valueRow;
    Array<U32> localRow;
    U32 originRowCount = 0;

    BitMatrix localIn;
    BitMatrix localOut;
    BitMatrix loanIn;
    BitMatrix loanOut;

    /*
     * One row of the local origin state, replaced by one node - the undo log `OriginCursor` walks.
     *
     * `position` is the index within the block of the node that replaced the row, and `row` is which
     * row it replaced. The contents the row held *before* that node are `logPrevious.row(entry)`,
     * which is what makes the log an undo log rather than a redo one: a forward walk needs no log at
     * all, because stepping forward is `stepOriginsForward` and that is the transfer itself.
     */
    struct OriginEdit {
        U32 position;
        U32 row;
    };

    /*
     * The state at every program point, as the block entries plus the edits between them.
     *
     * This used to be a table - `nodeLocals`, a row of `originLocalCount x loanWords` words per *run*
     * of nodes between two writes. It was the largest thing the analysis allocated and it was a product of
     * three terms that all grow with the program: 640 origin-carrying locals written four times each
     * is 3201 runs x 640 rows x 50 words, which is 819 MB - past what a region can address, for a
     * body of seven thousand lines. `test/bench/ownership-scaling.py` shape A is that measurement and
     * `test/bench/findings.md` §72 is what it reported.
     *
     * The compression run there was the right observation and the wrong table. `stepOriginsForward`
     * replaces **one** row and leaves every other row of the state alone, so consecutive runs differ
     * in one row of `loanWords` words - measured, and it is exactly one at every one of the 3200
     * boundaries. Storing the whole state at each of them is therefore storing the same rows over
     * and over: the same body's edits are 3200 x 50 words, which is 1.3 MB.
     *
     * So the cross product is not stored at all. The block entry states are `localIn`, which the
     * fixpoint needs anyway; between them a walk carries one state and applies the edits, forwards
     * by running the transfer and backwards by restoring what the log kept. That is the same §8.6
     * trade `walkBlock` makes for move-path liveness and `checkLoans` makes for the live-loan set -
     * an answer each position uses once is recomputed along the walk rather than tabulated for all
     * of them - and this was the one table it had not been applied to.
     *
     * `logStart` has `blockCount + 1` entries, so a block's edits are the half-open range
     * `logStart[b]` to `logStart[b + 1]`, in node order.
     */
    Array<U32> logStart;
    Array<OriginEdit> log;
    BitMatrix logPrevious;

    // One value's positions, gathered before a write applies them. A buffer rather than a row
    // pointer into the state, because a write whose value comes out of the storage it writes - `t`
    // assigned from itself - would otherwise read rows it had already replaced.
    Array<U64> writeScratch;

    // How many origin positions a value has, and one of them. Zero positions is a value carrying no
    // origins at all, which is most of them.
    U32 valuePositions(NodeId node) const {
        return valueRow[node.index + 1] - valueRow[node.index];
    }

    U64* originsOfValue(NodeId node, U32 position) {
        return valueOrigins.row(valueRow[node.index] + position);
    }

    U64* loansAt(BitMatrix& table, U32 blockIndex) { return table.row(blockIndex); }

    // One whole local origin state - a block's entry or exit row, or the one a cursor carries. All
    // three have the same stride because they hold the same thing at three points.
    Size localWidth() const { return Size(originRowCount) * loanWords; }
    U64* localsAt(BitMatrix& table, U32 index) { return table.row(index); }

    // One position of one local within such a state. Whole words per position, which is what lets
    // the answer be a row in its own right: every caller treats it as one loan set and unions it
    // into another.
    U32 localPositions(LocalId local) const {
        return localRow[local.index + 1] - localRow[local.index];
    }

    U64* originsOfLocal(U64* locals, LocalId local, U32 position) {
        return locals + Size(localRow[local.index] + position) * loanWords;
    }

    // The root of a projection chain - the local, global or address the place is reached through.
    Place* rootOf(PlaceId place);

    // The leading field steps from the root local down to a place, and whether the walk left this
    // frame's own storage - see `Projection`.
    void projectionOf(PlaceId place, Projection& into);

    /*
     * Which origin positions of a local a place reaches, and which position of the value *at* the
     * place each one becomes.
     *
     * Three cases and each is one sentence. The local's own root is always reached, because reaching
     * anything through the local reaches it through whatever the local itself refers to. A slot the
     * projection walks *into* is reached, and what comes out is the object that slot named - so it
     * lands on the result's root. A slot the projection stops *above* survives as a slot of the
     * result, at the path that is left after the projection is taken off the front. Anything that
     * diverges from the projection is not reached at all, and that last one is the whole point:
     * `t.right` says nothing about `t.left`.
     */
    void selectPositions(const Projection& projection, const Occurrence& local,
                         const Occurrence& at, SmallArray<PositionMap, 8>& into);

    // The position in `schema` of the slot at `slot.path` with its first `from` steps removed, or
    // the root where the schema has no such position. The other half of `selectPositions`' third
    // case, and the one place a path is matched against a schema rather than against a projection.
    U32 slotAt(DeclPtr<OwnershipSchema> schema, ExternalSlot& slot, U32 from);

    // Whether an operand supplies any origins at all, asked *structurally* - from the occurrence and
    // never from the state, because the log's shape has to be the same on every visit to a block.
    bool operandCarriesOrigins(Operand operand);

    /*
     * Every origin position one *operand* supplies - the origin environment asked the only way four
     * passes all have to ask it.
     *
     * A value operand supplies its own positions and a place operand supplies the positions of the
     * storage behind it, mapped through the projection, and both are uses of the same lender:
     * `idOf(d)` names a value and `return d.n` names a place, and a rule that saw only the first let
     * the loan behind `d` die between them. One function rather than the pair inlined at each call
     * site, because they are answers that have to agree - the origin environment builds it, liveness
     * keeps the lender alive by it, and the extent walk ends the loan by it.
     *
     * The cursor is the point the operand is read at, and it is not optional: storage answers this
     * question differently at different points, so a caller that had no point to name would be
     * asking about the union again.
     */
    template<class F>
    void forEachOperandOrigin(OriginCursor& cursor, Operand operand, F&& f);

    // The same, flattened - the loans an operand may reach at all, whichever position holds them.
    // What the extent walk and move-path liveness want, since neither asks *which* part of a value
    // reached the lender.
    void unionOperandOrigins(OriginCursor& cursor, Operand operand, U64* into);
    void unionPlaceOrigins(OriginCursor& cursor, PlaceId place, U64* into);

    template<class... T>
    void report(LocationId source, StringView text, T&&... arguments) {
        errors++;
        context.diagnostics.error(text, source, forward<T>(arguments)...);
    }

    MovePath& path(MovePathId id) { return *out.path(base, id); }

    /*
     * The path of a teardown's subject - the parameter this body is the destruction of - or none.
     *
     * It is a path like any other and is *not* released like any other, which is the one thing this
     * field records. The subject arrives owned and is dead by the end, so the ordinary rule would
     * place a teardown for it, and the teardown of a `T` is this very function; and its members are
     * not released either, because `doc/spec/ownership.md` has an authored answer *replace* the
     * derived walk rather than run beside it.
     *
     * So the subtree is walked and nothing in it is dropped. What the walk is for is the rule the
     * authored answer takes on by existing: **an authored `Drop` is answerable for its subject's
     * linear members**. A member the body neither disposes of nor moves out would otherwise be
     * released by nobody at all, silently - and for an `@explicitDrop` member that silence is
     * exactly the implicit disposal the attribute was written to prevent.
     *
     * This used to be "the subject has no path", which cost two things beyond the check: `->x` on a
     * field of it reported that the storage was not this function's, and a use after a partial move
     * inside a teardown was not followed at all.
     */
    MovePathId teardownSubject;

    bool underTeardownSubject(MovePathId id) {
        if(!teardownSubject.valid()) return false;
        for(auto up = id; up.valid(); up = path(up).parent) {
            if(up == teardownSubject) return true;
        }

        return false;
    }

    Block* block(U32 index) { return body.block(base, BlockId(index)); }

    /*
     * Whether a flattened lens's frame is still standing here, and therefore whether its storage is
     * still live - see `FrameLocal`. Two readings of one rule, because liveness is asked at two
     * granularities and a drop is placed from the finer one.
     *
     * `keepFrameLocals` is the edge: an edge that stays inside the frame carries its storage across,
     * and one that leaves does not. `holdFrameLocals` is inside a block: every position but the
     * terminator, so that the storage is live through the callee's own statements and dead at the
     * one instruction that leaves - which is where the release then lands, after everything the
     * callee wrote and before control goes anywhere.
     */
    void keepFrameLocals(U64* live, U32 from, U32 to);
    void holdFrameLocals(U64* live, U32 index);
    U32 nodeCount(U32 index) { return block(index)->nodes.size(); }
    Node& nodeAt(U32 blockIndex, U32 position) {
        return *base[block(blockIndex)->nodes.get(base, position)];
    }

    // The paths, in an order where a parent always precedes its children - which is the order they
    // are created in, since a field is interned after the record it projects.
    U32 pathWords = 0;
    U64* liveAt(BitMatrix& table, U32 blockIndex) { return table.row(blockIndex); }
    PathState* stateAt(Array<PathState>& table, U32 blockIndex) {
        return table.pointer() + blockIndex * pathCount;
    }

    /*
     * The blocks in reverse postorder, and where each one sits in it.
     *
     * Every fixpoint here is driven from this rather than from block index order, and the two are
     * not the same thing: index order is the order the *builder* emitted blocks in, and a forward
     * dataflow driven by it re-derives a loop header's entry state after the body has already been
     * walked with the old one. Reverse postorder settles an acyclic region in one sweep and a loop
     * in one sweep per level of nesting.
     *
     * Unreachable blocks are appended in index order rather than dropped. They are IR someone built,
     * `checkLoans` and `walkBlock` walk them, and a table row nothing ever wrote would be read by
     * those - so they are visited with the entry state they have, which is empty.
     *
     * The order alone and no position table beside it: nothing here asks where a block sits, only
     * what to visit next. The SSA verifier's own copy keeps positions because its dominator
     * computation compares them.
     */
    Array<U32> order;
    void computeOrder();

    // Every block the terminator of `index` names, in edge order. A callback rather than a list,
    // because two of the three callers want one block at a time and the third is a stack walk that
    // asks for the same list at every step.
    template<class F>
    void forEachSuccessor(U32 index, F&& f) {
        auto block = this->block(index);
        auto count = block->nodes.size();
        if(!count) return;

        auto last = base[block->nodes.get(base, count - 1)];
        forEachEdge(*last, [&](Edge& edge) {
            if(edge.target) f(base[edge.target]->index.index);
        });
    }

    /*
     * The worklist itself - which blocks still have to be visited.
     *
     * A round-based sweep re-visits every block in the body after any one of them changes, which is
     * the `R x N` in every bound over these analyses. This visits a block when something it reads
     * has moved and not otherwise: a sweep in the right order picks up everything woken ahead of the
     * cursor within the same sweep, so the ordinary body settles in one.
     */
    IndexSet pending;

    void wake(U32 index) { if(index < blockCount) pending.set(index, true); }

    void wakeSuccessors(U32 index);
    void wakePredecessors(U32 index);

    // `visit(index)` over the pending blocks until none are left, in reverse postorder for a forward
    // analysis and in the reverse of it for a backward one. Seeded with every block, because each
    // has to be visited at least once whatever it reads.
    template<class Visit>
    void sweepForward(Visit&& visit) {
        pending.reset(blockCount);
        pending.fill();

        for(auto working = true; working; ) {
            working = false;

            for(auto index: order) {
                if(!pending.get(index)) continue;

                pending.set(index, false);
                working = true;
                visit(index);
            }
        }
    }

    template<class Visit>
    void sweepBackward(Visit&& visit) {
        pending.reset(blockCount);
        pending.fill();

        for(auto working = true; working; ) {
            working = false;

            for(Size i = order.size(); i > 0; i--) {
                auto index = order[i - 1];
                if(!pending.get(index)) continue;

                pending.set(index, false);
                working = true;
                visit(index);
            }
        }
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
    void buildOriginLog();
    void computeLoanExtents();
    bool checkLoans();

    /*
     * One node's write into the local origin state, resolved once.
     *
     * The transfer and the undo log ask the same question and they have to get the same answer: the
     * log records what a node overwrote, so a row the transfer touches and the log did not is a row
     * a backward walk restores to the wrong contents. Resolving it in one place rather than twice is
     * what makes that a property of the code instead of a comment.
     *
     * Everything on it is **structural** - the node, the occurrences and the schema, never the state
     * - because the log is counted before the fixpoint climbs and a shape that moved as the analysis
     * settled would be undone against a different body.
     */
    struct StateWrite {
        LocalId local;
        Projection projection;
        Occurrence localOccurrence;
        Occurrence placeOccurrence;
        U32 placeCount = 0;

        // Whether what was there is discarded - `replacedLocal`, carried so that the transfer states
        // the condition once.
        bool strong = false;

        SmallArray<PositionMap, 8> positions;
    };

    bool localWriteOf(Node& node, StateWrite& into);

    /*
     * One node's effect on local origin state - the forward transfer `computeOrigins` iterates.
     *
     * Written once and applied to a running state rather than folded into the fixpoint's switch,
     * because the *shape* of the update is the decision: a write into storage this frame owns
     * replaces what is there, and a write through a loan does not. Getting that backwards is a
     * silent unsoundness in one direction and a false rejection in the other.
     */
    void stepOriginsForward(OriginCursor& cursor, Node& node);

    /*
     * The place a node replaces the whole of, or null - the strong update's condition, named once.
     *
     * A write to a whole local this frame **owns** discards what was there. Two passes need to know
     * that and they need to agree: the forward transfer replaces the local's origins with the
     * written value's, and the extent walk stops counting the destination as a use. A partial write,
     * a write through a loan and a write to anything but a local are none of those, and the same
     * answer says so to both.
     */
    Place* replacedLocal(Node& node);

    // One node, backwards, over the loan set. Shared by the extent fixpoint and by the check walk,
    // which needs the answer at every position rather than at every block - the same arrangement
    // `applyBackward` has for move paths, and for the same reason.
    void stepLoansBackward(U64* live, OriginCursor& cursor, Node& node);

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
    void markUse(U64* live, MovePathId id, bool exactly);
    void markKill(U64* live, MovePathId id);
    void useOfPlace(U64* live, PlaceId place);
    void applyBackward(U64* live, OriginCursor& cursor, Node& node);

    // The name a diagnostic calls a path by: the root local's, which is what the author wrote.
    String pathName(MovePathId id);

    /*
     * Whether this path *is* the temporary a `->x` wrote its value into - `Local::disposal`.
     *
     * The root and nothing else: a disposal writes a whole value into a fresh local, so the local is
     * the path, and a field of it could only be reached by a projection nothing in the body has a
     * name for. Asked at the one point a value dies unasked, which is the one point the difference
     * between "the compiler chose here" and "the source said here" is the whole question.
     */
    bool writtenDisposal(MovePathId id);

    /*
     * `@local` on the teardown that is about to run - `doc/spec/ownership.md`'s "who may dispose".
     *
     * Checked where the teardown is *placed* rather than at `->x`, and that is the point: an
     * implicit release at last use triggers the instance exactly as an explicit disposal does, and
     * a check that only saw the written one would let the guard be released by saying nothing.
     *
     * Direct instances only. The transitive walk - a container whose derived teardown reaches a
     * `@local` one - is the next step of `doc/proposal/Design-Linear-Types.md`'s build order, and it
     * is a walk over the same member graph `Summary`'s reachability already makes.
     */
    void checkDisposalPermission(LocationId source, MovePathId id);

    /*
     * Records that this body ends a value of a type it does not know - `Disposal` in `decl.h`.
     *
     * The other half of `checkDisposalPermission`, and beside it because they are one question asked
     * of the two kinds of type a body can hold: one this walk can see, and one only a substitution
     * decides. `implicit` is whether the *compiler* chose the point, which is what `@explicitDrop`
     * is about; `@local` is about the disposal happening at all, so it does not read the flag.
     */
    void deferDisposal(DeclPtr<Type> type, LocationId source, bool implicit);

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
 * The local origin state at one program point, carried along a walk.
 *
 * Every pass after `computeOrigins` asks "what is this storage rooted in *here*", and the answer
 * used to be a table row indexed by the node. It is not a table any more - see `Analysis::log` for
 * the measurement that took it out - so the question is asked of a walk that is standing at the
 * point, which is what every one of those passes already was.
 *
 * That costs nothing, because none of them wanted random access. All five walk one block at a time
 * and in one direction: `computeOrigins` and `checkLoans`'s check pass go forwards, the extent
 * fixpoint, move-path liveness and `checkLoans`'s live-set pass go backwards. A cursor is seeded at
 * whichever end the walk starts from and stepped with it.
 *
 * Going **forwards** needs no log: stepping over a node is `stepOriginsForward`, which is the
 * transfer itself and is the same function the fixpoint runs. Going **backwards** is where the log
 * is spent - the state before a node is the state after it with the row that node replaced put back,
 * and that row is what the log kept.
 *
 * `revision` counts the times the state has moved. It is what `checkLoans` keys its closure cache
 * on: the closure is taken from the state, so it is the same closure until the state changes, and a
 * counter says that without the cache having to know what a run is.
 */
struct OriginCursor {
    explicit OriginCursor(Analysis& analysis);

    Analysis& analysis;

    // The state itself - `localWidth()` words, refilled at each block rather than allocated per
    // block, because a walk visits every block and the width does not change between them.
    Array<U64> buffer;

    U32 blockIndex = 0;

    // The next log entry to consume, which is one past the last edit already undone. A backward walk
    // starts it at the end of the block's range and works down.
    U32 entry = 0;

    U32 revision = 0;

    U64* state() { return buffer.pointer(); }

    // Seeded at the top of a block, for a walk that runs forwards from there.
    void enter(U32 block);

    // Seeded at the bottom of one, for a walk that runs backwards. The exit state is `localOut`,
    // which `buildOriginLog` wrote from this very walk's forward twin - so the two ends of the block
    // are one answer rather than two that have to agree.
    void enterAtExit(U32 block);

    /*
     * Undoes the node at `position`, leaving the state the one on *entry* to it.
     *
     * Called before the node is read rather than after, because a backward walk is standing above
     * the node it is about to look at: at the bottom of a block the state is the block's exit, which
     * is the state after the last node, and the last node's own operands were read under the state
     * before it.
     *
     * A loop rather than an `if`, because a node replaces one row *per position*: a write to a whole
     * `Reader` replaces what the value refers to and what its `held` field refers to, which is two
     * entries at one position and the same walk. `localWriteOf` is what decides how many.
     */
    void undo(U32 position);
};

/*
 * The origin environment, asked of one operand - defined here because it needs a cursor to read the
 * state through and `Analysis` cannot see one yet where it is declared.
 */
template<class F>
void Analysis::forEachOperandOrigin(OriginCursor& cursor, Operand operand, F&& f) {
    if(!loanCount) return;

    if(operand.source() == SourceKind::Result) {
        auto node = operand.nodeId();
        auto count = valuePositions(node);
        for(U32 p = 0; p < count; p++) f(p, (const U64*)originsOfValue(node, p));
        return;
    }

    if(operand.source() != SourceKind::Place) return;

    auto place = operand.placeId();
    auto root = rootOf(place);
    if(root->kind != Place::Local) return;

    auto local = LocalId(root->payload);
    if(!localPositions(local)) return;

    Projection projection;
    projectionOf(place, projection);

    SmallArray<PositionMap, 8> map;
    selectPositions(projection, body.local(base, local)->occurrence,
                    body.place(base, place)->occurrence, map);

    // The state where the cursor is standing, and not a whole-body union: which storage a local
    // refers to is a property of the point it is read at - see `Analysis::log`.
    for(auto& entry: map) {
        f(U32(entry.to), (const U64*)originsOfLocal(cursor.state(), local, entry.from));
    }
}

/*
 * Whether a value of this type is tracked at all.
 *
 * A type that releases nothing and duplicates for free has no move to record and no teardown to
 * place, so a path for one would be a row in every table for a fact nothing reads. Both halves are
 * asked because they are different reasons: a teardown means the end of the value's life is
 * observable, and a refusal to duplicate means a read of it is a move.
 */

} // namespace mir
