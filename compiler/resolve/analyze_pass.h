#pragma once

#include "analyze.h"
#include "expr.h"
#include "witness.h"

/*
 * What the ownership analyses share.
 *
 * analyze.h states what the passes are and what a caller gets out of them; this is the state they
 * are written against, and it is private to the analyze_*.cpp files:
 *
 *   analyze.cpp             the per-function driver and the interprocedural fixpoint.
 *   analyze_effects.cpp     the numbering every other pass indexes by, and what each instruction
 *                           does to the locals it touches.
 *   analyze_live.cpp        liveness, and the live ranges the dump prints.
 *   analyze_own.cpp         the ownership lattice.
 *   analyze_provenance.cpp  which storage each value may refer to, and what each root contains.
 *   analyze_escape.cpp      what has to outlive the frame, and the storage class that follows.
 *   analyze_demand.cpp      what each root's representation has to be able to do.
 *   analyze_summary.cpp     what all of the above says to a caller.
 *   analyze_borrow.cpp      the four checks: moves, exclusivity, return roots, closure captures.
 *   analyze_drop.cpp        where the drops go, and the rewrite that puts them there.
 *   analyze_teardown.cpp    the glue a drop names, and the shape rule an authored `Reclaim` obeys.
 *   analyze_print.cpp       the dump.
 *
 * The division is one file per pass rather than one per phase, because a pass is what a regression
 * is localized to: a drop in the wrong place is either liveness or placement, and those are now two
 * files that share nothing but the state below.
 *
 * ---------------------------------------------------------------------------------------------
 * What every pass works over, gathered once.
 *
 * The numbering is the spine: each instruction of the function gets one index - blocks in the order
 * they were built, phis before instructions before the terminator - and liveness, ownership state
 * and drop points are all stated in those indices. That is the same arrangement
 * lower/lower_analyze.cpp uses at the lower level, deliberately, so the two dumps read against each
 * other.
 *
 * Sets are a byte per local rather than a bit. A function has a handful of locals, the sets are
 * copied constantly by the fixpoint, and a byte array copies correctly by value while a packed one
 * would need care every time - which is the wrong thing to spend care on here.
 */

using LocalSet = Array<U8>;

struct BlockRange {
    U32 first = 0;
    U32 end = 0;
};

// What one instruction does to the locals it touches. `defs` and `uses` drive liveness, `moves`
// drives the ownership lattice, and `overwrites` keeps a slot's old contents live up to the write
// that replaces them - which is where the drop for those contents goes.
struct Effects {
    // Writes that replace the whole slot, so nothing above them can still be reaching its old
    // contents. These end a live range going backwards.
    Array<U32> defs;

    // Writes that make a slot owned without replacing all of it - one field of an aggregate being
    // constructed. They are `uses` for liveness, because the rest of the slot survives them, and
    // this list only records the ownership half.
    Array<U32> inits;

    Array<U32> uses;
    Array<U32> moves;

    /*
     * Slots a whole-slot `Assign` replaces the contents of.
     *
     * A use for liveness and nothing else. The old value has to still be live *into* the write for
     * the write to be the point its lifetime ends at - without that the slot reads as dead from its
     * last real use onwards, the last-use rule drops it there, and the overwrite rule drops it again
     * a few instructions later. It is deliberately not a `use` for the move check, because writing a
     * slot that was moved out of is how one is filled again rather than a use of what left it.
     */
    Array<U32> overwrites;
};

/*
 * Which storage one value may refer to.
 *
 * Ownership is stated over places, but everything a *caller* needs to know is stated over values:
 * whether the pointer this function returned points into its own frame, whether the borrow it was
 * handed ended up somewhere that outlives the call. So each value carries the set of roots it may
 * refer to, as an ordinary forward fixpoint over the SSA graph.
 *
 * The set is over locals rather than over "argument or not", because the two questions the result
 * answers are different: a summary asks which *arguments* a value is rooted in, and storage-class
 * selection asks which *allocations* have to outlive the frame. A local backed by an Arg answers
 * the first, every local answers the second, and one set covers both.
 */
struct Provenance {
    LocalSet locals;
    bool global = false;

    // Storage this analysis cannot name: the result of an opaque call, or anything reached through
    // a raw pointer whose own origin was already unknown. Conservative in the one direction that
    // matters - it can only make storage live longer than it had to.
    bool unknown = false;
};

struct Analysis {
    Analysis(Module& module, Function& function):
        module(module), context(module.context), global(*module.types), local(*module.arena),
        function(function) {}

    Module& module;
    Context& context;
    GlobalBase global;
    ModuleBase local;
    Function& function;

    Size localCount = 0;
    Size instructionCount = 0;
    Size valueCount = 0;

    Array<ModulePtr<Inst>> order;
    Array<BlockRange> blockRanges;
    Array<Effects> effects;
    Array<TrackedLocal> tracked;

    // Where each instruction sits in the numbering, so that a value's use list can be turned into
    // an extent without rescanning.
    HashMap<U32, U32> indexOf;

    Array<LocalSet> liveIn;
    Array<LocalSet> liveOut;

    // Ownership state before each instruction, one row per instruction index.
    Array<Array<OwnState>> stateBefore;

    /*
     * The flow facts, all keyed the same way: values by their id, roots by their local index.
     *
     * `contents` is what makes this more than a walk of the operand graph. A value written into a
     * place is reachable through that place's root afterwards, so an array's buffer is reachable
     * through the array, and returning the array is what makes the buffer outlive the frame. It is
     * field-insensitive - `x.a` and `x.b` contribute to one set - which is precise enough for the
     * question and avoids a second projection model inside the analysis.
     */
    Array<Provenance> values;
    Array<Provenance> contents;

    /*
     * Whether each root's storage has to stay valid after this frame returns.
     *
     * Two arrays for one question, because the two consumers ask it differently. `outlives` starts
     * with every parameter's slot set, since a parameter names the caller's storage and that
     * already survives - which is what makes "written into an argument" an escape with no rule of
     * its own. `escaped` records only what this pass *proved* escapes, which is what a summary
     * reports as a retained argument and what storage-class selection reads.
     */
    LocalSet outlives;
    LocalSet escaped;

    /*
     * The part of `escaped` that something else now *owns*.
     *
     * Escaping is one bit for the question "must this storage still be valid after the frame
     * returns", and two different answers for the question "who hands it back". A returned value's
     * contents, a member of an aggregate that left, an argument a callee consumed: those belong to
     * whatever they left with, and its teardown is what releases them. A pointer a call this pass
     * could not summarize may have kept is neither of those - the storage is still this frame's,
     * and this frame still has to release it.
     *
     * Only the second kind is an approximation, which is why the distinction is worth a set: the
     * frame that would leak it is the frame that never proved anything.
     */
    LocalSet transferred;

    // What each root's representation has to be able to do, per Design.md's owner mutation demand.
    Array<ReprRequirements> demand;

    // Which roots this frame has to hand storage back for. Not simply "is heap-placed": storage
    // that escaped is heap-placed *because* something else owns it now.
    LocalSet releasesStorage;

    // Reported diagnostics, but also the switch that decides whether this run is one of the
    // fixpoint's silent rounds or the final one that rewrites the body.
    bool reporting = true;
    bool ok = true;

    // Whether this run is the one that gets to change the program - insert drops, and generate the
    // glue those drops name. A silent round computes the same facts and keeps them to itself.
    bool rewriting = true;

    Block* blockAt(Size index) { return local[function.blocks.get(local, index)]; }
    Size blockCount() { return function.blocks.size(); }
};

/*
 * Numbering and effects (analyze_effects.cpp).
 */

// Gives every instruction of the function its index, blocks in the order they were built, and
// records where each block's run of them starts and ends.
void numberFunction(Analysis& analysis);

// What each instruction does to the locals it touches, plus the two corrections that make the
// answer the one the CFG has: a value that refers into a slot keeps that slot alive wherever the
// value is used, and a phi's operands are used on the edges into it rather than at the phi.
void computeEffects(Analysis& analysis);

/*
 * Liveness and its printed form (analyze_live.cpp).
 */

void computeLiveness(Analysis& analysis);
void buildRanges(Analysis& analysis, OwnershipResult& result);

// One backward step over the instructions in [first, end), in place. Exposed because the fixpoint
// only keeps liveness at each block's two ends, and both the drop placer and the range builder
// recover it inside a block by replaying exactly this.
void applyBackward(Analysis& analysis, Size first, Size end, LocalSet& live);

/*
 * The ownership lattice (analyze_own.cpp).
 */

void computeOwnership(Analysis& analysis);

/*
 * Provenance and containment (analyze_provenance.cpp).
 */

void computeProvenance(Analysis& analysis);

Provenance emptyProvenance(Size count);

// Adds everything in `source` to `target`, reporting whether that changed anything. Every fact in
// these three passes is a "may" fact climbing from empty, so this is the only way any of them move.
bool joinProvenance(Provenance& target, const Provenance& source);

Provenance& provenanceOf(Analysis& analysis, ModulePtr<Value> value);

// Whether a value is the kind of thing that can refer to storage at all. A scalar computed into a
// register refers to nothing, and saying so keeps arithmetic out of the fixpoint entirely.
bool refersToStorage(Analysis& analysis, TypePtr type);

// The roots a place names, and what reading out of one produces.
Provenance placeProvenance(Analysis& analysis, const Place& place);
Provenance contentsOfPlace(Analysis& analysis, const Place& place);

// What a value contributes when it is written somewhere. An aggregate is copied byte for byte, so
// what lands in the destination is what the source contained rather than the source itself.
Provenance transferredProvenance(Analysis& analysis, ModulePtr<Value> value);

// The summary of a called function, or nothing when the callee is not one this pass can see.
FunctionSummary* summaryOf(Analysis& analysis, ModulePtr<Function> callee);

/*
 * Escape, and the storage class it decides (analyze_escape.cpp).
 */

void computeOutliving(Analysis& analysis);
void selectStorage(Analysis& analysis, OwnershipResult& result);

/*
 * Owner mutation demand (analyze_demand.cpp).
 */

void computeDemand(Analysis& analysis);

/*
 * The summary (analyze_summary.cpp).
 */

// Rebuilds this function's summary from the current round's facts, reporting whether anything moved.
bool deriveSummary(Analysis& analysis);

/*
 * The checks (analyze_borrow.cpp).
 */

void checkMoves(Analysis& analysis);
void checkBorrows(Analysis& analysis);
void checkReturnRoots(Analysis& analysis);
void checkClosureEnvironments(Analysis& analysis);

/*
 * Drop placement and the rewrite (analyze_drop.cpp).
 */

// Decides where the drops go and inserts them, splitting an edge where one belongs on an edge.
// Reports rather than emitting for the two shapes that would need a drop flag.
void insertDrops(Analysis& analysis);

/*
 * Teardown glue (analyze_teardown.cpp).
 */

// The implementation one type's teardown half runs, or null where that half has nothing to do.
ModulePtr<Function> teardownFor(Module& module, TypePtr type, Teardown half, LocationId source);

// Whether an authored `Reclaim` stays inside what Design-Memory §4 lets one do.
bool checkReclaimShape(Module& module, Function& function);

/*
 * Small shared helpers.
 */

// A set with a slot per local, all clear.
LocalSet emptySet(Size count);

// The local a place is rooted in, or none. A global outlives every function and a raw pointer's
// target is outside the ownership model by definition, so neither contributes.
U32 rootLocal(Analysis& analysis, const Place& place);

/*
 * The local a value is the contents of, or none.
 *
 * An aggregate that lives in storage is named by the value that produced it - a call result, a copy,
 * an allocation - and Function::locals records that pairing, which is what lets an SSA operand be
 * recognized as an owned slot. It is also how a parameter's slot is found, since a parameter is
 * named by its Arg exactly as an allocation is named by its Alloc.
 */
U32 backingLocal(Analysis& analysis, ModulePtr<Value> value);

// Whether this slot is a parameter's - storage the caller named, which arrives already holding a
// value and already outlives the frame. Asked by five of the passes and by the summary.
inline bool isParameterSlot(Analysis& analysis, Size local) {
    auto slot = analysis.function.localAt(analysis.local, U32(local));
    return slot.value && analysis.local[slot.value]->kind == Value::Arg;
}

// One argument's bit in a declared or actual return-root group. Positions past 64 have no bit and
// are silently outside every group, which is the conservative direction: an undeclared root is
// reported rather than believed.
inline U64 rootBit(U16 index) {
    return index < 64 ? U64(1) << index : 0;
}

/*
 * Every diagnostic this file produces goes through here, because the passes run more than once.
 *
 * Summaries are a fixpoint: a function is analyzed as many times as it takes for what its callees
 * say about themselves to stop changing, and only the last of those rounds is the one whose
 * diagnostics are the program's. Reporting from the silent rounds would say the same thing three
 * times; not recording `ok` in them would let a round that failed still be treated as a result.
 */
template<class... Args>
inline void report(Analysis& analysis, StringView text, LocationId source, Args&&... args) {
    analysis.ok = false;
    if(analysis.reporting) {
        analysis.context.diagnostics.error(text, source, forward<Args>(args)...);
    }
}

inline void note(Analysis& analysis, StringView text, LocationId source) {
    if(analysis.reporting) {
        analysis.context.diagnostics.message(Diagnostics::MessageLevel, text, source);
    }
}
