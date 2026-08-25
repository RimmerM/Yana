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
 *   analyze_borrow.cpp      the checks: moves, exclusivity, return roots, and loan extents.
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
 * Sets are a bit per local, and every one of them is borrowed from AnalysisScratch rather than
 * allocated. Both halves of that are measurement rather than taste: the sets used to be `Array<U8>`
 * built by pushing one zero at a time, which made the empty set alone the single largest source of
 * allocations in the compiler - and a set is built per block per round by two fixpoints and per
 * temporary by a third, so what matters is not how big one is but how many are made.
 */

// What the ownership passes call an IndexSet when its members are the function's locals, which is
// everywhere except the range builder. See util/container.h for the set itself.
using LocalSet = IndexSet;
using LocalSetList = IndexSetList;

struct BlockRange {
    U32 first = 0;
    U32 end = 0;
};

// What one instruction does to the locals it touches. `defs` and `uses` drive liveness, `moves`
// drives the ownership lattice, and `overwrites` keeps a slot's old contents live up to the write
// that replaces them - which is where the drop for those contents goes.
/*
 * Three of the five lists below are `ArrayF` rather than `SmallArray`, which is a claim and not a
 * guess: a fixed array has no heap to spill to, and in a release build the overflow check compiles
 * away, so the bound has to hold by construction.
 *
 * It does, and deriveEffects is the whole of why. Each of `defs`, `inits` and `overwrites` is pushed
 * from one or two guarded sites in that one switch, at most once each per instruction, and nothing
 * else writes them - `extendBorrowUses` adds only to `uses` and `attributePhiEdges` only to `uses`
 * and `moves`. That last one is exactly why those two are not fixed: a terminator collects one entry
 * per phi alternative on its outgoing edges, which the program decides rather than this file.
 *
 * A case added to that switch is the thing that would break this, and it breaks loudly: the bound is
 * asserted in debug builds, which is what the fixture suite runs.
 */
struct Effects {
    // Writes that replace the whole slot, so nothing above them can still be reaching its old
    // contents. These end a live range going backwards. At most two: the value's own slot, and the
    // place a whole-slot write names.
    ArrayF<U32, 2> defs;

    // Writes that make a slot owned without replacing all of it - one field of an aggregate being
    // constructed. They are `uses` for liveness, because the rest of the slot survives them, and
    // this list only records the ownership half.
    ArrayF<U32, 2> inits;

    SmallArray<U32, 4> uses;
    SmallArray<U32, 2> moves;

    /*
     * Slots a whole-slot `Assign` replaces the contents of.
     *
     * A use for liveness and nothing else. The old value has to still be live *into* the write for
     * the write to be the point its lifetime ends at - without that the slot reads as dead from its
     * last real use onwards, the last-use rule drops it there, and the overwrite rule drops it again
     * a few instructions later. It is deliberately not a `use` for the move check, because writing a
     * slot that was moved out of is how one is filled again rather than a use of what left it.
     */
    ArrayF<U32, 1> overwrites;

    // Emptied rather than destroyed, because there is one of these per instruction and the list
    // holding them belongs to the program - see PooledList and AnalysisScratch.
    void clear() {
        defs.clear();
        inits.clear();
        uses.clear();
        moves.clear();
        overwrites.clear();
    }
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

    /*
     * The parameters this refers to that have no slot in this frame to be named by - see
     * computeProvenance's seeding, and `rootBit` for the encoding.
     *
     * A parameter gets a local only where the frame has storage for it: a `&` binding, or an
     * argument of memory type. A `&Int` or a `%U8` arrives in a register, so it has neither - and a
     * copy of an address still names the caller's storage, which is the whole of why a raw pointer
     * is exempt from `arrivesAsCopy`. Without a bit here those two parameters had *no* provenance
     * at all, so a result built out of one looked rooted in nothing: `fn make(x: &Int, y: &Int) ->
     * Pair(&Int, &Int)` was accepted with no `return` group and handed the caller two references
     * its signature never mentioned, and `fn passThrough(p: %U8) -> %U8 = p` the same.
     *
     * A bitmask over argument indices rather than a set over locals, because that is the shape the
     * one consumer wants: `FunctionSummary::actualRoots` is already a mask over the same indices,
     * so this reaches it by an `|=` rather than through a slot-to-argument lookup.
     *
     * Not consulted by the escape pass, and correctly so. What escape places is storage this frame
     * allocated, and a parameter with no slot allocated nothing here; what it *refers* to is the
     * caller's, which is exactly what the return-root contract is for.
     */
    U64 args = 0;

    bool global = false;

    // Storage this analysis cannot name: the result of an opaque call, or anything reached through
    // a raw pointer whose own origin was already unknown. Conservative in the one direction that
    // matters - it can only make storage live longer than it had to.
    bool unknown = false;

    // Empties this one and sizes it for a function with `count` locals, keeping its buffer. Every
    // provenance in the passes below is produced this way rather than constructed.
    void reset(Size count) {
        locals.reset(count);
        args = 0;
        global = false;
        unknown = false;
    }
};

// The same arrangement as LocalSetList, for the two rows the provenance fixpoint keeps: one per
// value and one per root.
struct ProvenanceList {
    void reset(Size rows, Size count) {
        while(sets.size() < rows) sets.push(Provenance());
        for(Size i = 0; i < rows; i++) sets[i].reset(count);
        used = rows;
    }

    Provenance& operator [] (Size index) { assertTrue(index < used); return sets[index]; }
    const Provenance& operator [] (Size index) const { assertTrue(index < used); return sets[index]; }
    Size size() const { return used; }

private:
    Array<Provenance> sets;
    Size used = 0;
};

/*
 * Every buffer the ownership passes work in, kept for the length of the compilation.
 *
 * `Analysis` is built and thrown away per function, and the program has around five hundred of them
 * before it has any of its own - so what these hold is not one function's working set but the
 * largest function's, reached once and then reused. Owned by `Program` and reached through the
 * module, so that nothing here is a global and a second compilation in one process shares nothing
 * with the first.
 */
// One drop the drop pass decided to insert. `before` is a linear index: the drop goes immediately
// before that instruction, which is always a real position because a terminator never defines or
// last-uses a local itself. Here rather than in analyze_drop.cpp because the scratch holds a list of
// them - see AnalysisScratch::blockDrops.
struct PendingDrop {
    U32 local = 0;
    U32 before = 0;

    // Set for a drop that releases what a write is about to replace, in which case the place comes
    // from the write rather than from the local - see makeOverwriteDrop.
    ModulePtr<Inst> overwrite = nullptr;

    // Set where the lattice said `Maybe` here, so whether this teardown runs is a run-time question
    // - see elaborateFlaggedDrops, which is what turns one of these into a branch.
    bool conditional = false;
};

// The drops each block needs, one row per block. Four inline: a block that drops more than that is
// not one this bound decides anything about.
using DropList = ArrayList<PendingDrop, 4>;

/*
 * One write of a drop flag, in the same shape and the same numbering as a pending drop.
 *
 * The flag is the ownership lattice made into a value, so a write of one goes exactly where
 * transferState changes the state it stands for: an allocation empties the slot, an init fills it,
 * and a move empties it again. `before` is the position immediately after the instruction whose
 * effect this records, which is why it is a position rather than an instruction.
 */
struct PendingFlag {
    U32 flag = 0;
    U32 before = 0;
    bool value = false;

    /*
     * The flag's own allocation, on the one entry per flag that declares it.
     *
     * That entry produces two instructions rather than one - the storage and the write that fills
     * it - and it is in the entry block, so a flag is allocated once however many times a loop
     * around it runs. Every later write of the same flag is an `Assign` with nothing here.
     */
    ModulePtr<Inst> allocation = nullptr;
};

using FlagList = ArrayList<PendingFlag, 4>;

struct AnalysisScratch {
    ~AnalysisScratch() {
        for(auto set: borrowed) delete set;
    }

    LocalSetList liveIn;
    LocalSetList liveOut;

    // Liveness at each point inside one block, which the drop placer recovers by replaying the
    // backward walk the fixpoint only kept the two ends of.
    LocalSetList blockLiveness;

    /*
     * The three single sets the passes work in.
     *
     * `work` is the live set a backward walk carries; `occupied` and `positions` are the range
     * builder's, and are over instruction indices rather than over locals. Named rather than
     * borrowed from the pool because each is held across a whole pass rather than an expression.
     */
    IndexSet work;
    IndexSet occupied;
    IndexSet positions;

    LocalSet outlives;
    LocalSet escaped;
    LocalSet transferred;
    LocalSet releasesStorage;

    ProvenanceList values;
    ProvenanceList contents;

    // The ownership lattice's two tables: the state before each instruction, and the state each
    // block is entered with. Four values each rather than one bit, so they are arrays of states.
    ArrayList<OwnState> stateBefore;
    ArrayList<OwnState> blockEntry;
    Array<OwnState> walkState;

    /*
     * The numbering and what it is indexed by, which every pass above reads.
     *
     * Here for the same reason as the rest: `order` and `blockRanges` are one array each per
     * function, `effects` is five per instruction, and none of them is anything but the previous
     * function's storage re-sized. `Analysis` names them as references so the passes are unchanged.
     */
    Array<ModulePtr<Inst>> order;
    Array<BlockRange> blockRanges;
    PooledList<Effects> effects;
    Array<TrackedLocal> tracked;
    Array<ReprRequirements> demand;
    HashMap<U32, U32> indexOf;

    // Where the drops each block needs are collected - see insertDrops. One row per block, which is
    // the shape ArrayList exists for; here rather than in the pass because a list built per function
    // is a list allocated per function.
    DropList blockDrops;

    // And the flag writes, in the same shape and for the same reason. A separate list rather than a
    // second kind of entry in the one above, because the two are placed by different rules and only
    // meet where both are spliced into a block.
    FlagList blockFlags;


    /*
     * The temporaries, handed out by ScratchProvenance.
     *
     * A pool with a depth rather than one named set per helper, because the helpers nest - what a
     * place names is asked while composing what reading out of it produces - and the depth is what
     * keeps two live borrows from being the same set. Held by pointer so that growing the pool
     * cannot move a set something is already holding a reference to.
     */
    Array<Provenance*> borrowed;
    Size borrowDepth = 0;
};

struct Analysis;

/*
 * A provenance borrowed for the length of one expression.
 *
 * Every helper below that used to return a `Provenance` by value now fills one of these instead,
 * because each of those returns was an allocation and the fixpoints make one per instruction per
 * round. The borrow is released by scope exit, so the next round reaches the same storage.
 */
struct ScratchProvenance {
    explicit ScratchProvenance(Analysis& analysis);
    ~ScratchProvenance();

    ScratchProvenance(const ScratchProvenance&) = delete;
    ScratchProvenance& operator = (const ScratchProvenance&) = delete;

    Provenance& operator * () const { return *set; }
    Provenance* operator -> () const { return set; }

private:
    AnalysisScratch& scratch;
    Provenance* set;
};

struct Analysis {
    Analysis(Module& module, Function& function);

    Module& module;
    Context& context;
    GlobalBase global;
    ModuleBase local;
    Function& function;

    // The buffers this run works in, which belong to the program rather than to this run. Bound as
    // references so that the passes still read `analysis.liveIn` and never have to know that.
    AnalysisScratch& scratch;

    Size localCount = 0;
    Size instructionCount = 0;
    Size valueCount = 0;

    Array<ModulePtr<Inst>>& order;
    Array<BlockRange>& blockRanges;
    PooledList<Effects>& effects;
    Array<TrackedLocal>& tracked;

    // Where each instruction sits in the numbering, so that a value's use list can be turned into
    // an extent without rescanning. The scratch's, like the rest: one entry per instruction, emptied
    // and refilled per function - see HashMap::reset.
    HashMap<U32, U32>& indexOf;

    LocalSetList& liveIn;
    LocalSetList& liveOut;

    // Ownership state before each instruction, one row per instruction index.
    ArrayList<OwnState>& stateBefore;

    /*
     * The flow facts, all keyed the same way: values by their id, roots by their local index.
     *
     * `contents` is what makes this more than a walk of the operand graph. A value written into a
     * place is reachable through that place's root afterwards, so an array's buffer is reachable
     * through the array, and returning the array is what makes the buffer outlive the frame. It is
     * field-insensitive - `x.a` and `x.b` contribute to one set - which is precise enough for the
     * question and avoids a second projection model inside the analysis.
     */
    ProvenanceList& values;
    ProvenanceList& contents;

    /*
     * Whether each root's storage has to stay valid after this frame returns.
     *
     * Two arrays for one question, because the two consumers ask it differently. `outlives` starts
     * with every parameter's slot set, since a parameter names the caller's storage and that
     * already survives - which is what makes "written into an argument" an escape with no rule of
     * its own. `escaped` records only what this pass *proved* escapes, which is what a summary
     * reports as a retained argument and what storage-class selection reads.
     */
    LocalSet& outlives;
    LocalSet& escaped;

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
    LocalSet& transferred;

    // What each root's representation has to be able to do, per Design.md's owner mutation demand.
    Array<ReprRequirements>& demand;

    // Which roots this frame has to hand storage back for. Not simply "is heap-placed": storage
    // that escaped is heap-placed *because* something else owns it now.
    LocalSet& releasesStorage;

    // Reported diagnostics, but also the switch that decides whether this run is one of the
    // fixpoint's silent rounds or the final one that rewrites the body.
    bool reporting = true;
    bool ok = true;

    // Whether this run is the one that gets to change the program - insert drops, and generate the
    // glue those drops name. A silent round computes the same facts and keeps them to itself.
    bool rewriting = true;

    /*
     * Set for `reselectStorage`'s run, which is this analysis asked one question a second time.
     *
     * `selectStorage` is the only pass it changes, and it changes it to a single rule: an allocation
     * the ownership stage put on the heap, which this run finds does not escape, moves to the frame.
     * Nothing else is written - not the drops, not the summary, not the demand - because everything
     * else was decided on facts inlining cannot have changed.
     *
     * The direction is the whole of what makes the re-run safe. Inlining removes escape reasons and
     * never adds them, since a callee's summary was already a conservative over-approximation of its
     * body, so this run's answer is a subset of the first one's. A promotion would mean the first
     * answer was wrong; a demotion this run *misses* is a heap allocation that stays one.
     */
    bool demoteOnly = false;

    Block* blockAt(Size index) { return local[function.blocks.get(local, index)]; }
    Size blockCount() { return function.blocks.size(); }

    /*
     * Whether a slot holds something this frame already owns before its first instruction runs.
     *
     * Exactly one thing does: a `->` parameter. The handover happened at the call site - the caller
     * recorded it as an `InstMove` - so what this frame received is its own to release, and it is
     * its own from the top rather than from an `init` somewhere in the body. Every other owned slot
     * is an allocation or a construction that this walk can see being initialized.
     *
     * A parameter is recognized the way every pass recognizes one: its slot is named by an `Arg`,
     * exactly as an allocation's is named by its `Alloc`.
     */
    bool ownedOnEntry(U32 index) {
        if(index >= function.localCount()) return false;

        auto slot = function.localAt(local, index);
        if(!slot.value || local[slot.value]->kind != Value::Arg) return false;

        return slot.convention == ast::BindType::Sink && !slot.borrowed;
    }
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

// Adds everything in `source` to `target`, reporting whether that changed anything. Every fact in
// these three passes is a "may" fact climbing from empty, so this is the only way any of them move.
bool joinProvenance(Provenance& target, const Provenance& source);

Provenance& provenanceOf(Analysis& analysis, ModulePtr<Value> value);

// Whether a value is the kind of thing that can refer to storage at all. A scalar computed into a
// register refers to nothing, and saying so keeps arithmetic out of the fixpoint entirely.
bool refersToStorage(Analysis& analysis, TypePtr type);

/*
 * The roots a place names, and what reading out of one produces.
 *
 * Each fills a set the caller owns rather than returning one, and every caller borrows that set
 * from AnalysisScratch - see ScratchProvenance. These are called once per instruction per round by
 * two fixpoints, so a returned value here is an allocation per instruction per round.
 */
void placeProvenance(Analysis& analysis, const Place& place, Provenance& into);
void contentsOfPlace(Analysis& analysis, const Place& place, Provenance& into);

// What a value contributes when it is written somewhere. An aggregate is copied byte for byte, so
// what lands in the destination is what the source contained rather than the source itself.
void transferredProvenance(Analysis& analysis, ModulePtr<Value> value, Provenance& into);

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

// Everything borrowed that outlived what was keeping it valid: a slice of this frame's container, a
// widened `&` temporary, a captured reference in an escaping closure, and the parameters a class
// implementation promised on behalf of instances no call site can see. One statement with five
// reasons - see the comment on the definition, and Analysis-Borrows.md §8.4 for what replaces it.
void checkLoanExtents(Analysis& analysis);

/*
 * Drop placement and the rewrite (analyze_drop.cpp).
 */

// Decides where the drops go and inserts them, splitting an edge where one belongs on an edge and
// building the flag and the branch where the lattice could not settle whether one runs.
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

/*
 * The slot a view is ultimately a view of - see Local::viewOf.
 *
 * A slice's descriptor is a local of this frame whatever it describes, so a returned one looks
 * rooted in the frame until this is walked: `elements(return self: Array(a)) -> &[a]` builds a
 * descriptor out of `self` and hands it back, and what the caller has to keep alive is `self`.
 *
 * The chain rather than one step, because a subslice is a view of a view; the depth bound is against
 * a cycle a rewrite could introduce, since nothing in the resolver builds one. Identical in shape to
 * analyze_effects' useSlot, which spends the same edge on liveness.
 */
inline U32 viewedRoot(Analysis& analysis, U32 local) {
    for(auto depth = 0; depth < 8; depth++) {
        auto next = analysis.function.localAt(analysis.local, local).viewOf;
        if(next == maxLimit<U32> || next >= analysis.localCount) break;

        local = next;
    }

    return local;
}

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
