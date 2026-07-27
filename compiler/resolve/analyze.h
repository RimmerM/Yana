#pragma once

#include "module.h"
#include "Net/Stream.h"

/*
 * The ownership analyses (Implementation-IR.md part 5).
 *
 * Four passes, run per function once its body is resolved. Each needs the one before it:
 *
 *   1. Liveness       - backward, over the function's locals: where each one's value can still be
 *                       reached by some path through the CFG.
 *   2. Ownership      - forward, over the same locals: is this slot initialized here, moved out of
 *                       here, or one of the two depending on which path arrived.
 *   3. Borrow check   - use after move, and exclusivity of `&`, both stated in terms of 1 and 2.
 *   4. Drop insertion - InstDrop where liveness ends, with a flag where ownership says the answer
 *                       depends on the path taken.
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
 * rejected rather than approximated. See checkMove().
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
 * with the drops it owes. Returns false when something was reported.
 *
 * The result is produced for printing rather than for any later stage: by the time lowering runs,
 * every decision these passes made is an instruction in the body.
 */
bool runOwnership(Module& module, Function& function, OwnershipResult& result);

// Runs the analyses over every function of the program with a body, in module order. Called once,
// after every body of every module is resolved, because a generic function's specializations only
// exist then - and it is the specializations, not the generic body, that get drops.
bool runProgramOwnership(Program& program);

// Writes the analysis result for the whole program, in the same golden-file spirit as the resolve
// and lower IR dumps. This is what makes liveness inspectable rather than only trusted.
void printOwnership(Net::Writer& writer, Context& context, Program& program);
