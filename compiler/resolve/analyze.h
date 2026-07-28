#pragma once

#include "module.h"
#include "Net/Stream.h"

/*
 * The ownership analyses (Implementation-IR.md part 5).
 *
 * Seven passes, each needing the one before it:
 *
 *   1. Liveness       - backward, over the function's locals: where each one's value can still be
 *                       reached by some path through the CFG.
 *   2. Ownership      - forward, over the same locals: is this slot initialized here, moved out of
 *                       here, or one of the two depending on which path arrived.
 *   3. Provenance     - which storage each *value* may refer to, and what each root contains. This
 *                       is the one the interprocedural half is built on: a summary is a statement
 *                       about where things a caller handed over ended up.
 *   4. Escape         - which roots have to stay valid after the frame returns, which is what
 *                       chooses between the frame and the heap.
 *   5. Demand         - what each root's representation has to be able to do, per Design.md's owner
 *                       mutation demand.
 *   6. Borrow check   - use after move, exclusivity of `&`, and the return-root rules, all stated
 *                       in terms of the five above.
 *   7. Drop insertion - InstDrop where liveness ends, releasing storage where the frame owns it.
 *
 * The first five are also what a function's summary is derived from, and a summary is what the
 * *next* function's copies of them read - so the whole thing runs to a fixpoint over the program
 * before the round that reports and rewrites. See runProgramOwnership.
 *
 * Everything is stated over storage rather than over values, because that is what ownership is
 * about: "the value in %v3" and "the storage `x` owns, which is currently borrowed" are different
 * things and only the second has a lifetime.
 *
 * ---------------------------------------------------------------------------------------------
 * Granularity, and what that costs.
 *
 * Liveness and ownership are tracked per *local*, not per place. A local is live if any part of it
 * is read, and it is moved if any part of it is moved. Borrow conflicts are the exception and are
 * compared at full place granularity, since `x.a` and `x.b` genuinely do not conflict and that is
 * the case `&` parameters run into immediately.
 *
 * The consequence is that moving one field out of an aggregate cannot be represented - the slot
 * would be half-owned, and every later drop of it would need to know which half. That is real work
 * (a drop flag per field, and a drop that runs on a subset of members), so a partial move is
 * rejected rather than approximated. See checkMoves(), and the full list of what this pass leaves
 * out at the end of analyze.cpp.
 * ---------------------------------------------------------------------------------------------
 */

// Where one local is live, as a half-open range over the function's linear instruction numbering.
// A local's liveness is a *list* of these rather than one interval: a value used on one arm of a
// branch and again after the join is genuinely dead in between, and Design.md's drop rule - "the
// first program point from which no path through the CFG reaches another use of it" - is stated
// exactly on that hole.
struct LiveRange {
    U32 from = 0;
    U32 to = 0;
};

// What a local owns at a program point, as a forward lattice. The join of two different states is
// `Maybe`, which is precisely the question a drop flag exists to answer at run time.
enum class OwnState: U8 {
    Uninitialized,
    Owned,
    Moved,
    Maybe,
};

// One local, as the analysis sees it.
struct TrackedLocal {
    TypePtr type = nullptr;
    StringId name = 0;

    // False for the slot behind a `&` parameter: storage the caller owns, which this frame must
    // neither drop nor move out of.
    bool owned = true;

    // Whether the end of this local's life has to run anything. Locals that do not are still
    // tracked, because use-after-move applies to every type and not only to droppable ones.
    bool droppable = false;

    // What this root's representation has to be able to do, and where its storage had to come from
    // to satisfy that. Both are Milestone 6's, and both are printed rather than only believed:
    // a wrong demand shows up as a heap allocation that did not have to happen, which is invisible
    // in the IR and obvious in the dump.
    ReprRequirements requirements;
    StorageClass storage = StorageClass::Stack;

    // Whether this frame proved the storage has to stay valid after it returns. Distinct from
    // `storage` because a parameter's slot outlives the frame without this frame having allocated
    // anything - the answer is the caller's.
    bool escapes = false;
};

struct OwnershipResult {
    Array<TrackedLocal> locals;

    // Live ranges for local `i`, at [rangeOffsets[i], rangeOffsets[i] + rangeCounts[i]).
    Array<LiveRange> ranges;
    Array<U32> rangeOffsets;
    Array<U32> rangeCounts;

    Buffer<LiveRange> rangesOf(Size local) const {
        auto pointer = const_cast<LiveRange*>(ranges.pointer());
        return Buffer<LiveRange> { pointer + rangeOffsets[local], rangeCounts[local] };
    }
};

// Every function's result, keyed by the function's own arena offset.
struct OwnershipResults {
    HashMap<U32, OwnershipResult> functions;
};

/*
 * Runs every ownership pass over one function, reporting what it rejects and rewriting the body
 * with the drops it owes and the storage classes it chose. Returns false when something was
 * reported.
 *
 * Every summary this reads has to be settled first, so a caller that is not runProgramOwnership
 * gets whatever the summaries currently say - which for an unvisited callee is the conservative
 * answer rather than a wrong one.
 *
 * The result is produced for printing and for lowering's Repr decision. Every other decision these
 * passes made is an instruction in the body by the time lowering runs.
 */
bool runOwnership(Module& module, Function& function, OwnershipResult& result);

// Runs the analyses over every function of the program with a body: silently until no summary
// moves, then once more to report and rewrite. Called after every body of every module is
// resolved, because a generic function's specializations only exist then - and it is the
// specializations, not the generic body, that get drops.
bool runProgramOwnership(Program& program);

// Which half of a teardown a request is about - see Design-Memory §4. The two are elidable under
// different conditions, so nothing may ask for "the teardown" without saying which.
enum class Teardown: U8 {
    Drop,
    Reclaim,
};

// The implementation one type's teardown half runs - an authored instance, or the glue synthesized
// for a derived one - or null where that half has nothing to do. Exposed because a TypeDesc names
// both halves and is built outside this pass; `module` is where the instances are looked up and
// where generated glue lands.
ModulePtr<Function> teardownImplementation(Module& module, TypePtr type, Teardown half, LocationId source);

// Writes the analysis result for the whole program, in the same golden-file spirit as the resolve
// and lower IR dumps. This is what makes liveness inspectable rather than only trusted.
void printOwnership(Net::Writer& writer, Context& context, Program& program);
