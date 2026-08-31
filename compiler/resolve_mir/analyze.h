#pragma once

#include "place.h"

namespace mir {

struct Function;
struct Program;

/*
 * The ownership analysis - Analysis-IR.md §6.2, §6.3 and §6.7.
 *
 * Three questions over one structure, which is the point of putting them behind one entry point:
 * they are three readings of the same move-path lattice and the same CFG fixpoint, and the current
 * compiler's equivalents are separate passes with separate notions of what a "value" is that have
 * disagreed about all three.
 *
 *  - **Liveness** (`analyze_live.cpp`) answers where each path is last used, and with that answer
 *    resolves every `UseMode::Forward` into the `Take` it was or the diagnostic it is. This is the
 *    one rewrite a sealed body permits, and it is why `Forward` exists at all (§5.4).
 *  - **Initialization** (`analyze_drop.cpp`) is the forward dataflow over move paths: use before
 *    initialization, use after move, double move, initialization over live contents, and which paths
 *    are only conditionally owned at a join.
 *  - **Drop placement** (`analyze_drop.cpp`) falls out of the two: an object is released at the
 *    first point at which it is owned and no longer live. That one sentence covers the end of a
 *    scope, the arm of an `if` that did not use it, the assignment that replaces it and the value
 *    returned from a loop, and stating it once is what stops those from becoming four rules.
 *
 * The result is the `Elaboration` below, in the MIR region, read by the SSA conversion - which emits
 * the teardown rather than a pass rewriting MIR to hold it (§9.2).
 */

/*
 * One initialization-tracked prefix of a place - §5.10.
 *
 * Created only where the function does something that distinguishes it from its parent: a local
 * whose type owes a teardown or refuses duplication gets one, and so does a field of such a local
 * that is itself in that class. A record used only as a whole has one path however many fields it
 * has, and a field of a record that is copied bit for bit has none - there is no operation on it
 * that its parent's state could not answer for.
 *
 * `place` is what it tracks and is what a diagnostic names. The tree is the place tree restricted to
 * field projections: an index or a dereference collapses to its container, which is §5.2's
 * conservative alias rule read from the initialization side.
 */
struct MovePath {
    MovePathId id;
    MovePathId parent;

    PlaceId place;
    DeclPtr<Type> type = nullptr;

    // In field order, and only the fields that are tracked. Non-embedding, because `MovePathId`'s
    // "none" is all ones and `SmallList`'s embedded encoding claims the high bit of an element.
    MirList<MovePathId, false> children;
};

/*
 * One teardown the conversion has to emit, and where.
 *
 * `position` is an index into the block's node list and means **before** that node: a drop is placed
 * at the point the object stopped being needed, and the node standing there is the one that made it
 * so - the `return`, the assignment that replaces it, the first instruction of the arm that does not
 * use it. `position == node count` is the end of a block that has no terminator, which a sealed body
 * does not have; every real drop therefore lands before a real node.
 *
 * `conditional` is a path that was owned on some incoming paths and not on others - §5.10's `Maybe`.
 * The conversion reads the path's drop flag and branches; `Elaboration::flagged` is what says a path
 * has one at all.
 */
struct DropStep {
    DropStep(): position(0), conditional(0) {}

    DropStep(U32 block, U32 position, MovePathId path, bool conditional):
        block(block), position(position), conditional(conditional), path(path) {}

    U32 block = 0;

    /*
     * The position, and the flag in the bit above it - twelve bytes rather than sixteen.
     *
     * A `bool` after two `U32`s and an ID is one byte and three of padding, and there is one of
     * these per teardown the analysis places in the whole program: the flag costs four bytes to say
     * one thing that has a free bit next to it. Thirty-one bits of position is a body with two
     * billion nodes in one block, which is past what the `U32` node index itself allows.
     *
     * A constructor pair rather than default member initializers, because those are not allowed on a
     * bit-field before C++20 and this is built at C++17. The two of them are what keep the fields
     * initialized, which an `= default` would not.
     */
    U32 position : 31;
    U32 conditional : 1;

    MovePathId path;
};

static_assert(sizeof(DropStep) == 12, "a drop step is a block, a position with its flag, and a path");

/*
 * One drop-flag write.
 *
 * `position` is the index of the node **after** which the flag changes, because a flag records what
 * the node just did: an initialization sets it and a move clears it. `position == kEntry` is the
 * function's own entry, where a parameter arrives owned and a local does not.
 */
struct FlagStep {
    // The largest a thirty-one bit position can be, rather than all ones - see `DropStep` for why
    // the field is thirty-one bits. A block with that many nodes is not reachable; a sentinel that
    // did not fit in the field would be.
    static constexpr U32 kEntry = (U32(1) << 31) - 1;

    FlagStep(): position(0), value(0) {}

    FlagStep(U32 block, U32 position, MovePathId path, bool value):
        block(block), position(position), value(value), path(path) {}

    U32 block = 0;

    // The position and the value it writes, packed on `DropStep`'s terms and for its reason.
    U32 position : 31;
    U32 value : 1;

    MovePathId path;
};

static_assert(sizeof(FlagStep) == 12, "a flag step is a block, a position with its value, and a path");

/*
 * What the analysis concluded about one body.
 *
 * In the MIR region, because it is read by the SSA conversion and dies with the rest of the body's
 * data. A side table rather than fields on the nodes, which is §8.6: a drop point is one pass's
 * answer, and hanging it off every node would widen every node in the program for its sake.
 *
 * The two step lists are sorted by `(block, position)`, so the conversion walks them with one cursor
 * each instead of searching per node.
 */
struct Elaboration {
    RegionArray<MirRegion, MirPtr<MovePath>> paths;
    RegionArray<MirRegion, DropStep> drops;
    RegionArray<MirRegion, FlagStep> flags;

    // One byte per path: whether it is ever dropped conditionally and therefore needs a flag.
    RegionArray<MirRegion, U8> flagged;

    /*
     * The origin environment - Analysis-IR.md §5.11's `OriginEnvironment(value)`.
     *
     * One bit per (node, loan): whether the value that node produces may be rooted in that loan,
     * `loanWords` words to a node. A bitmap rather than a set per value because a body has tens of
     * loans and hundreds of nodes, and every reader of it is a union or a membership test.
     *
     * Kept past the analysis for one reason: it is the only thing that says *why* a borrow is
     * legal, so a dump that showed loans and not what a value is rooted in would be a golden that
     * could not distinguish a correct signature conformance check from one that passed everything.
     * Empty where the body takes no loans, which is most of them.
     */
    U32 loanCount = 0;
    U32 loanWords = 0;
    RegionArray<MirRegion, U64> origins;

    MovePath* path(MirBase base, MovePathId id) const {
        return base[paths.get(base, id.index)];
    }

    // Whether the value `node` produces may be rooted in `loan`. Answers false for a body with no
    // loans without touching the table, which is what makes the caller's loop free there.
    bool rootedIn(MirBase base, NodeId node, LoanId loan) const {
        if(!loanCount) return false;

        auto word = origins.get(base, node.index * loanWords + loan.index / 64);
        return (word >> (loan.index % 64)) & 1;
    }
};

/*
 * Runs the analysis over one sealed body, filling `Body::elaboration`.
 *
 * Returns false and reports where the *program* is wrong - a use after a move, a value that cannot be
 * copied consumed twice - which is the one thing separating this from the verifiers: those report
 * the compiler's own mistakes as internal errors, and these are the diagnostics the language is
 * specified to give.
 */
bool analyzeBody(Context& context, Program& program, Function& function);

// Every reachable-or-not body, in declaration order. Reachability is not known yet: this runs before
// `computeSummaries`, because the summary's own reachability walk follows the drops this places.
bool analyzeProgram(Context& context, Program& program);

} // namespace mir
