#include "gen.h"
#include "x64_util.h"

/*
 * Placement.
 *
 * Every web is given one location - a register, a slot in the frame, or a recipe that recreates the
 * value wherever it is needed - for the whole of its life, and keeps it. Nothing is ever relocated
 * mid-function, so a value is in the same place on every path that reaches a given instruction,
 * which is what makes the result independent of how the blocks happen to be laid out.
 *
 * This pass answers one question and no other: *where does each value persist*. What that then means
 * at any one instruction - which operand is read out of the frame, which is copied into the register
 * an encoding demands, where a result is written before being carried home - is legalization's, and
 * it runs afterwards over a placement that is already complete (legalize.cpp).
 *
 * The separation is what makes a displacement cheap. A web placed here can be reconsidered while
 * placement is still running, because nothing downstream has been written yet: no instruction names
 * a location until legalization puts one there. The pass used to place and emit in one walk, so a
 * displaced web meant re-running everything.
 *
 * The inputs both come from lower_analyze.cpp: a linear numbering of the instructions (in the order
 * LowerFunction::blocks lists the blocks, which transformFunction has put in reverse postorder), and
 * a live interval per value in that numbering. Two webs may share a location exactly when their
 * intervals never overlap.
 *
 * A web that cannot be given a register does not get moved around to make one available. It is given
 * one of the two homeless states instead - a slot in the frame, or a recipe that recreates it
 * wherever it is read - and legalization brings it into a scratch register at the instructions that
 * cannot work with it where it is.
 *
 * The one thing a web *is* moved for is a clobber it would otherwise have to dodge for the whole of
 * its life. A web live across a call may keep a caller-saved register on either side of it and step
 * into the frame just across the call, rather than paying for a register safe over its whole life -
 * or, where there is no such register, for no register at all. That is the only split this produces,
 * and §"Splitting" below is what decides when it is worth it.
 *
 * The pipeline is four passes:
 *
 *   0. buildWebs         - phi-related values that provably never overlap become one web, so the
 *                          copy between them is an identity and disappears.
 *   1. computeAvoidSets  - which registers each web has to stay out of, because something writes
 *                          them while the web is live.
 *   1b. computeSpillCosts - what each web would cost in either homeless state, so that a register
 *                          can be taken from whichever web values it least.
 *   2. placeValues       - one walk of the function placing every web, in the order legalization
 *                          will later ask about them.
 *
 * Phis are ordinary values here. A phi that shares a web with the value arriving over an edge needs
 * nothing at all; otherwise its location is decided at the first predecessor edge that reaches it.
 *
 * The result is checked before it is used: verifyPlacement confirms that every live web has a legal
 * location and that no two values whose lives overlap were given the same one, and verify.cpp then
 * checks the legalized instructions against it. Both run in debug builds only.
 */

/*
 * Range sets.
 *
 * A live range, a web's merged ranges and the stretches one location has already been promised to
 * are all the same thing - a sorted, disjoint list of program-point ranges - so they are all handled
 * as one, and LiveInterval's overlap test serves all three.
 */

// Merges `b` into `a`, keeping the result sorted and disjoint. Both are already sorted, so this is
// one merge walk with adjacent ranges folded together.
//
// The merge cannot run in place - it reads `a` while writing the result - so it runs through a
// buffer the caller lends it and copies back, which is what keeps a range list that has already
// been grown from being replaced by a fresh one on every merge.
template<class Ranges>
static void mergeRanges(Ranges& a, const Range* b, Size count, Array<Range>& out) {
    out.clear();
    Size i = 0, j = 0;

    auto append = [&](Range range) {
        if(out.size() > 0 && range.from <= out[out.size() - 1].to) {
            auto& last = out[out.size() - 1];
            if(range.to > last.to) last.to = range.to;
        } else {
            out.push(range);
        }
    };

    while(i < a.size() && j < count) {
        if(a[i].from <= b[j].from) append(a[i++]);
        else append(b[j++]);
    }

    while(i < a.size()) append(a[i++]);
    while(j < count) append(b[j++]);

    a.clear();
    for(auto& range: out) a.push(range);
}

template<class Ranges>
static LiveInterval intervalOf(const Ranges& ranges) {
    return LiveInterval { ranges.pointer(), U32(ranges.size()) };
}

/*
 * Webs.
 *
 * Placement is over webs rather than over values. A web is a set of values that a phi ties together
 * and that provably never overlap, so one location serves all of them - and the copy the phi would
 * otherwise need becomes `r <- r` and disappears.
 *
 * This is what a loop-carried value costs without it: the phi at the header and the value computed
 * for the next iteration are two SSA names for one quantity, and each iteration pays a move to get
 * from one to the other. They do not actually overlap - the phi is dead from the point it is read to
 * the latch - so proving that and giving them one register removes the move outright.
 *
 * Merging is conservative: two webs join only when their intervals are shown not to overlap. There
 * is no optimistic merge-and-repair, which is a much larger piece of machinery for a case this
 * codebase does not have.
 */
struct WebInfo {
    // The union of the member intervals, kept sorted and disjoint so that the same overlap test
    // works on a web as on a single value. Inline for the two a value usually has: there is one of
    // these per value in the function, and a row's first growth is the allocation the pool cannot
    // save.
    SmallArray<Range, 2> ranges;

    // Registers no member of the web may be given, because something writes them while one of them
    // is live.
    RegSet avoid;

    // The part of `avoid` a split cannot buy back: registers written at an instruction that reads a
    // member of the web out of its home. Those have to survive the copy in front of the instruction
    // and whatever its expansion writes before reading its sources, and no window can help - the web
    // is read *there*, from wherever it lives, so wherever it lives has to be safe.
    //
    // Everything else in `avoid` comes from a clobber the web merely outlives, which is exactly what
    // a window carries it past. See planSplit.
    RegSet avoidFixed;

    // What this web costs if it does not get a register, by either of the two ways of not having
    // one - see computeSpillCosts. Both are in the same units, so they can be compared with each
    // other and with another web's.
    U32 spillCost = 0;
    U32 rematCost = 0;

    // Set when the web is one definition of a value cheap enough to recreate wherever it is read,
    // and `recipe` is how - see Remat in gen.h.
    bool canRemat = false;
    Remat recipe;

    // A register something that *reads* this web needs it in: the result register at a return, an
    // argument register at a call, the count in rcx at a shift. A web given that register to begin
    // with makes the copy legalization would emit there a copy from a register to itself, which is
    // the same saving copyHint buys from the other end - see computeAvoidSets.
    //
    // One register rather than a set, because only one such site can ever be satisfied unless they
    // all agree. `preferredWeight` is how often the site it came from runs, which is what picks
    // between them: a read inside a loop is worth more than the one on the way out of the function.
    MachineLocation preferred;
    U32 preferredWeight = 0;

    LiveInterval interval() const { return intervalOf(ranges); }

    // What losing its register would actually cost this web, which is whichever of the two homeless
    // states it would then choose. This is the number one web is weighed against another by.
    U32 homelessCost() const { return canRemat && rematCost < spillCost ? rematCost : spillCost; }

    // Empties the web for the next function without giving up the range list it grew - see
    // PlacementScratch. Everything a web knows is derived from the function being placed, so an
    // emptied one is the state the constructor used to produce.
    void clear() {
        ranges.clear();
        avoid = RegSet();
        avoidFixed = RegSet();
        spillCost = 0;
        rematCost = 0;
        canRemat = false;
        preferred = MachineLocation::invalid();
        preferredWeight = 0;
    }
};

// One clobbering instruction, remembered so that values whose ranges cross it can be kept out of
// the registers it writes. Collected in a first pass because a value has to be placed before the
// walk reaches the instructions it outlives.
struct ClobberSite {
    U32 index;
    RegSet mask;

    // What a value *read here* has to survive, which is not the same set: at a call the clobbers are
    // the callee's and the callee has not run when its operands are read, so only the fixed
    // registers the copy in front of the instruction writes are in the way. `mask` is what a web
    // living *across* the site has to dodge; this is what one whose last read is here has to dodge.
    // The same distinction computeAvoidSets draws, kept so that §5.9 can ask it of one instruction.
    RegSet operandMask;

    // The instruction itself and how often it runs, both for splitting: a window may not cover a
    // site that defines one of the web's own members, and what a window costs is two instructions
    // where this one stands.
    LowerInst* inst = nullptr;
    U32 weight = 1;

    // A terminator can be a site, and is never one a window may cover: the copy bringing the web
    // home is emitted in the *next* instruction's parallel copy, and there is no next instruction
    // in a block that has already branched.
    bool terminator = false;
};

/*
 * One web's claim on one register.
 *
 * Nearly every claim is a web's whole life, holes and all: the web is in that register wherever it
 * is live and the register is free for anything whose life is disjoint from it, which is the one
 * test `isFree` has ever made. A web that stepped out of the register over a window (§5.8) claims it
 * across the window too, since the copies at either end read and write it there.
 *
 * A **cluster** claim (§5.9) is the other kind, and it is the one that needed a range beside the web
 * id. Such a web lives in the frame and borrows a register over one stretch of one block, so what it
 * holds the register against is that stretch and nothing else - its life outside the stretch says
 * nothing about who may have the register there.
 */
struct RegisterClaim {
    LiveId web = LiveId(0);

    // Meaningful only when `partial`. Half-open, in program points, and one point wider at the low
    // end than the segment it stands for: the copy that establishes the register is emitted in the
    // *previous* instruction's parallel copy, so the register has to be nobody else's there too.
    Range stretch;

    // False for the whole-life claim above, where the web's own interval is the claim.
    bool partial = false;
};

/*
 * What splitAroundClusters works over - see §5.9, which is where all three of these are explained.
 * They are here because the scratch buffers below hold them.
 */

// A block, as the cluster search reads it: where it begins and ends in the linear numbering, and
// how often it runs. A window lives inside one of these, so every use in one is worth the same.
struct BlockSpan {
    U32 firstIndex = 0;
    U32 lastIndex = 0; // the terminator's index, which a window may not reach
    U32 weight = 1;
};

// One instruction that touches a web, in the order the walk reaches them.
struct WebRead {
    U32 index = 0;
    U32 span = 0;

    // What reading this operand out of a register instead of out of the web's home would save, in
    // computeSpillCosts' units and unweighted. Zero for a definition, which is not a read at all but
    // a point no window may cover.
    U32 saving = 0;
    bool defines = false;
};

// A stretch of one block over which a web borrows a register, and the register it borrows.
struct ClusterWindow {
    Range range;
    MachineLocation at;
};

/*
 * What a homeless web costs, and what a split one does.
 *
 * Both are stated in quarter-instructions, so that "a longer encoding" can be told apart from
 * "free", and both are weighted by how often the block they are in actually runs. computeSpillCosts
 * prices the first; planSplit prices the second against it.
 */

static constexpr U32 kReloadCost = 4;    // an operand that has to be brought into a register
static constexpr U32 kStoreCost = 4;     // a result that has to be carried back to its slot
static constexpr U32 kRematCost = 4;     // recreating the value where it is read
static constexpr U32 kFoldedUseCost = 1; // an operand read straight out of the frame

// How much a split has to save before it is taken. Nothing beyond being cheaper, which is already a
// margin of one whole instruction: a window costs two - a store and a reload - and buys back one per
// read the web would otherwise have had to bring into a register, so the smallest split this accepts
// is one that turns three reloads into two instructions. Two would break even and is refused.
static constexpr U32 kSplitMargin = 0;

// "No price", for the comparison in `assign`: larger than any cost a function can accumulate, so an
// option that does not exist simply never wins.
static constexpr U32 kNoCost = ~U32(0);

// The most one block may be worth. Costs are summed over every read and write in a function in 32
// bits, so the weight has to leave room for that; five loop levels is already further in than any
// difference between two of them decides anything.
static constexpr U64 kMaxBlockWeight = 32768;

// "No register chosen", for the searches below. A value of its own rather than the length of the
// order they walk: the order holds only the registers the target hands out, so its length is a
// perfectly good register index and using it as a sentinel would read r15 as failure.
static constexpr Size kNoRegister = ~Size(0);

// "This instruction writes no register behind anyone's back", for the dense index §5.9 reads. A
// value of its own for the same reason kNoRegister is one: zero is a perfectly good site index.
static constexpr U32 kNoSite = ~U32(0);

/*
 * Allocation order.
 *
 * First-fit needs an order to be first in, and register number is the wrong one: it puts rbx fourth
 * and rbp sixth, both of which a function has to save and restore if it takes them, ahead of half a
 * dozen registers its convention lets it destroy for free.
 *
 * So the registers this function's own convention clobbers come first - a value in one of those
 * costs nothing - and the ones it owes its caller come last, in register order within each group.
 * Nothing has to be said about which values go where: a web that crosses a call already avoids
 * every register the call clobbers (computeAvoidSets), so the values that need a preserved register
 * are exactly the ones that cannot take anything from the first group.
 *
 * rbp goes last of all. It costs the same push and pop as any other preserved register, but a
 * function that puts a value in it also gives up being walkable through the frame-pointer chain, so
 * it is worth having only when the alternative is a frame slot - which is what being last means.
 *
 * The candidates are the bank's *allocatable* set rather than its register count: a register the
 * target never hands out has no business in an order whose whole purpose is to be tried in turn. Today
 * that is rsp alone, and skipping it here rather than filtering it out at every use is what keeps a
 * future bank that reserves k0, or the extended vector registers no encoding can name without EVEX,
 * from being offered and rejected sixteen times per web.
 */
static void buildOrder(const CallConvention& convention, RegisterBankId bank, U16* out, Size& outCount) {
    Size count = 0;
    auto framePointer = framePointerReg();
    auto& allocatable = targetRegisters().bank(bank).allocatable;

    // A bank's set holds only its own registers, which targetRegisters() checks - so the register this
    // yields is one of `bank`'s by construction.
    auto pass = [&](bool clobbered) {
        allocatable.iterate([&](PhysicalReg reg) {
            if(reg == framePointer) return; // last, below
            if(convention.clobber.has(reg) == clobbered) out[count++] = reg.index;
        });
    };

    pass(true);
    pass(false);

    if(allocatable.has(framePointer)) out[count++] = framePointer.index;

    assertTrue(count == allocatable.count()); // every allocatable register is in the order exactly once
    outCount = count;
}

/*
 * Everything a placement pass works in, held across the functions being placed rather than built
 * per function - see RegScratch in gen.h.
 *
 * Every buffer here is O(values) or O(registers) in size and empty again by the time the next
 * function starts, and there are eleven of them plus a list per value; building them per pass is
 * where a small module's compilation was spending most of its allocations. The one rule is that
 * nothing in here may carry meaning across a call: `reset` empties all of it, and a pass that read
 * something it had not written this time would be reading the previous function.
 */
struct PlacementScratch {
    PooledList<WebInfo> webs;
    // Which webs are in each register. Inline rows: there are a fixed few hundred of these and
    // each holds the handful of webs that shared one register, so pooling alone still left one
    // allocation per row per function that first reached it.
    SmallArray<RegisterClaim, 8> occupants[kRegisterBankCount][kMaxRegistersPerBank];
    ArrayList<LiveId> tieConflicts;
    ArrayList<Range> slotOccupants;
    ArrayList<LiveId> slotWebs;
    Array<ClobberSite> clobberSites;

    // Where the four walks get the instruction shape they each ask for per instruction. A pool
    // rather than one shape, because two of them nest - see Scratch.
    ScratchPool<InstShape> shapes;

    // buildWebs: the union-find parents, the conflicts while they are still stated over values, and
    // the block order the merges are attempted in.
    Array<LiveId> parent;
    ArrayList<LiveId> valueTies;
    Array<Size> blockOrder;

    // computeSpillCosts: how many values each web holds.
    Array<U16> members;

    // The buffer mergeRanges merges through, and the clobber sites planSplit found a web crossing.
    Array<Range> merged;
    Array<Size> crossed;

    // planSplit: the windows of the register being priced, and of the cheapest one it has seen.
    Array<Range> windows;
    Array<Range> bestWindows;

    // splitAroundClusters: which clobber site each instruction is, or kNoSite. Dense, because the
    // cluster search asks it of every instruction of a run rather than of a web's crossings, and one
    // index per instruction is what keeps that from being a search per instruction.
    Array<U32> siteOfInst;

    // The webs it is considering, the blocks it reads their weights and bounds out of, the reads of
    // each web in instruction order, and the windows it decided on for one web.
    Array<LiveId> clusterWebs;
    Array<U8> clusterWanted;
    Array<BlockSpan> spans;
    ArrayList<WebRead, 4> reads;
    Array<ClusterWindow> clusterWindows;

    void reset(Size valueCount) {
        webs.reset(valueCount);
        tieConflicts.reset(valueCount);
        slotOccupants.reset(0);
        slotWebs.reset(0);
        clobberSites.clear();

        for(auto& bank: occupants) {
            for(auto& reg: bank) reg.clear();
        }
    }
};

void destroyPlacementScratch(PlacementScratch* scratch) {
    delete scratch;
}

struct Placer {
    LowerBase base;
    LowerFunction& fun;
    Liveness& live;
    const MachineFunction& machine;
    const Constraints& constraints;

    PlacementScratch& scratch;

    // Where the four walks below get the instruction shape they each ask for per instruction. A
    // pool rather than one shape, because two of them nest - see Scratch.
    ScratchPool<InstShape>& shapes;

    // How often each block runs relative to the function's entry - see FunctionFrequencyInfo. Every
    // decision here that trades one part of the function against another is weighed by it: what a
    // web costs if it is left homeless, and which of two competing phi copies is worth coalescing.
    // Computed once per allocation rather than per placement pass, since nothing on the displacement
    // loop changes the CFG or the edge metadata it is derived from.
    const FunctionFrequencyInfo& frequency;

    // The result, built as the walk goes rather than copied out at the end - which is what lets the
    // operand rule below ask where a sibling operand will be read from while placement is still
    // running, against exactly the structure legalization will read afterwards. The caller's, so
    // that a second pass over the same function - and the next function after it - writes into the
    // buffers the first one grew.
    Placement& out;

    // What placement needs to know about each web and nothing downstream does: its merged interval,
    // what it has to stay out of, and what it would cost to leave homeless. Indexed alike with
    // `out.webs`, so a web id names both. Which web each value belongs to is `out.webOf`, which is
    // union-find while the webs are being built and a direct index once they are.
    PooledList<WebInfo>& webs;

    // Everything already placed in each register. A list rather than a single occupant because
    // intervals have holes: several webs can share one register over the function as long as no two
    // of them are ever live at the same point.
    SmallArray<RegisterClaim, 8> (&occupants)[kRegisterBankCount][kMaxRegistersPerBank];

    // The pairs of webs that may not share a register for a reason interval overlap does not state -
    // see collectTieConflicts. Indexed by web id, holding web ids, and symmetric: whichever of the
    // two is placed first, the second finds the register taken.
    //
    // Built by buildWebs, which needs the same relation to decide what it may merge. That it answers
    // both questions is the point: a destructive result and a sibling operand of its instruction may
    // no more share a register than share a web, and a rule that only refused the merge left the
    // register sharing to be discovered by whichever of the two happened to be placed second.
    ArrayList<LiveId>& tieConflicts;

    // The registers a value can be handed, held once rather than rebuilt at every assignment.
    RegSet allocatable = allocatableRegs();

    // The order to try them in, per bank - see buildOrder. Registers this function's own
    // convention lets it destroy come first, since taking one of those costs nothing at all.
    U16 order[kRegisterBankCount][kMaxRegistersPerBank] = {};
    Size orderCount[kRegisterBankCount] = {};

    // Every register this pass decided the function writes: the ones handed out to values, plus the
    // ones instructions clobber or are forced to write behind a value's back. A register that is
    // clobbered is just as destroyed from the caller's point of view as one holding a value, so both
    // sources count; the scratch registers legalization hands out are added to these afterwards.
    RegSet written;

    // Everything the function needs stack space for. Filled in as the reasons appear - an argument
    // the caller left on the stack, an alloca, a web that could not be given a register - and handed
    // to frame layout, which is what turns any of it into an address. Written straight into the
    // result, as are the four below: they are the placement's, not the placer's, and copying them
    // out at the end would be one buffer handed over and another thrown away.
    FrameObjects& frame;

    // What each spill slot has already been promised to, so that a slot can be reused whenever the
    // stretches do not overlap - the same rule that lets two webs share a register, and what keeps
    // the frame as small as the peak of simultaneously spilled values rather than as large as their
    // total. Ranges rather than webs, because a split web occupies its slot over its windows alone
    // and a homeless one over the whole of its life, and the slot cannot tell the two apart.
    ArrayList<Range>& slotOccupants;

    // Which webs those stretches belong to, which the ranges alone cannot say. Recycling a slot asks
    // the same two questions taking a register does - see Placer::isFree - and the second of them,
    // `tiesConflict`, is about a *pair of webs* and not about where either one lives. So the slot has
    // to remember its occupants by name and not only the stretches they cover.
    ArrayList<LiveId>& slotWebs;

    // Every instruction that writes registers behind its operands' backs, in index order - see
    // ClobberSite. Kept because a split is a decision about which of them a web has to dodge and
    // which it can step around.
    Array<ClobberSite>& clobberSites;

    // The recipes for the webs that live nowhere at all - see Remat in gen.h. A web's home names
    // its position here.
    Array<Remat>& remats;

    // Where each argument arrived, in argument order - see Placement::incomingArgs.
    Array<MachineLocation>& incomingArgs;

    // The registers each web has to keep out of, because a previous pass over this function found
    // something that wanted one of them more. Indexed by web id; see the displacement comment on
    // `assign` and the loop in allocateRegisters.
    //
    // A set rather than a flag, and that is the whole of what makes a displacement cost the displaced
    // web one register rather than all of them: the asking web needed *this* register free over
    // *this* interval, and nothing about the rest of the file follows from that. A web that used to
    // be told "take no register at all" spent the frame on registers nothing else had asked for -
    // measured on `each` in `Pipeline.yana`, three values in the frame with r14 free the whole way.
    const Array<RegSet>& displacedFrom;

    // The webs a previous pass displaced, which went homeless at their turn and are offered a
    // register again once the walk has finished - see `reclaimDisplaced`.
    Array<LiveId> deferred;

    // Webs *this* pass would rather have displaced than the one it displaced instead. Placement is
    // one walk in the order legalization will later read it, so a web already placed has already
    // been offered to everything that could have taken its register from it - the request is carried
    // out to allocateRegisters and applied to the next pass.
    Array<Placement::DisplacementRequest>& displacementRequests;

    // Set when legalizing this placement could need scratch registers - see the field of the same
    // name on Placement for the two things that set it. The first pass over a function reserves
    // none; if this comes back true, the reserve is measured and, if it grew, the function is placed
    // again with it held back.
    bool requiresLegalizationTemps = false;

    // How many webs have been placed in a register of a class the machine has no exchange for, per
    // bank. The second one is where a copy cycle becomes possible at all, and so where the reserve
    // has to be measured: one register cannot permute with itself.
    Size exchangeless[kRegisterBankCount] = {};

    // The scratch registers held back for this function, which no web may be given - and which the
    // rule below has to know the identity of, since a copy into one of them at an instruction is a
    // register a sibling operand of that instruction cannot be read out of.
    const TemporaryReserve& temporaries;

    Placer(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
        const Constraints& constraints, const FunctionFrequencyInfo& frequency, bool framePointer,
        const TemporaryReserve& temporaries, const Array<RegSet>& displacedFrom,
        PlacementScratch& scratch, Placement& out):
        base(base), fun(fun), live(live), machine(machine), constraints(constraints),
        scratch(scratch), shapes(scratch.shapes), frequency(frequency), out(out),
        webs(scratch.webs), occupants(scratch.occupants), tieConflicts(scratch.tieConflicts),
        frame(out.frame), slotOccupants(scratch.slotOccupants), slotWebs(scratch.slotWebs),
        clobberSites(scratch.clobberSites),
        remats(out.remats), incomingArgs(out.incomingArgs), displacedFrom(displacedFrom),
        displacementRequests(out.displacementRequests), temporaries(temporaries)
    {
        // Whatever is held back - the scratch registers, and rbp in a function that establishes a
        // frame pointer - is not available to hand out as a home.
        auto reserved = temporaries.regs();
        if(framePointer) reserved.add(framePointerReg());
        allocatable = reserved.complement(allocatable);

        auto& convention = constraints.getConvention(fun.callType);
        for(Size bank = 0; bank < kRegisterBankCount; bank++) {
            buildOrder(convention, RegisterBankId(bank), order[bank], orderCount[bank]);
        }

        // Both sides emptied together: the result and the placer's own tables are indexed alike by
        // web id, so one of them left holding a previous function's rows would be read as this
        // function's answer.
        auto valueCount = live.valueMap.size();
        out.clear();
        out.webs.reset(valueCount);
        scratch.reset(valueCount);

        for(Size i = 0; i < valueCount; i++) {
            out.webOf.push(LiveId(i));

            auto& web = webs[i];
            auto interval = live.getInterval(LiveId(i));
            for(U32 r = 0; r < interval.count; r++) web.ranges.push(interval.ranges[r]);
        }
    }

    LiveId webIdOf(LowerValue* v) {
        auto id = v->liveId();
        assertTrue(id != kNullLive); // every non-implicit value is numbered by buildLiveness
        return out.webOf[id];
    }

    WebInfo& webFor(LowerValue* v) { return webs[webIdOf(v)]; }

    // Whether two values ended up sharing a location, which is what makes the copy between them an
    // identity that never has to be emitted.
    bool sameWeb(LowerValue* a, LowerValue* b) { return webIdOf(a) == webIdOf(b); }

    // How much more one execution of this block is worth than one execution of the function's entry
    // - the block frequency, in units of the entry's own. Every decision that trades one part of the
    // function against another is weighed by it.
    //
    // Truncated to an integer rather than kept as a fraction, so a block that runs *less* often than
    // the entry weighs one, the same as the entry does. That is the resolution the costs are stated
    // at and there is nothing below it to distinguish: what matters is that a loop body outweighs
    // the code around it, and that a cold arm inside that loop does not.
    U32 weightOf(LowerBlock* block) const {
        auto scaled = frequency.frequencyOf(block->index) / kEntryFrequency;
        if(scaled < 1) return 1;

        return U32(scaled < kMaxBlockWeight ? scaled : kMaxBlockWeight);
    }

    // The weight of the block the placing walk is in, which is what a hint offered by the
    // instruction being placed is worth - see `assign`, where it is weighed against what a web's own
    // preference is worth. One entry's worth while the arguments are being placed, since the entry
    // block is what runs them.
    U32 currentWeight = 1;

    // Where a web lives, and whether it has been given anywhere yet. Both read the result directly:
    // a web's home is its first segment, so there is no second record of it to disagree. An unplaced
    // web answers an invalid location rather than asserting, because a hint offered before its
    // source has been reached is a hint that is simply not taken.
    bool placed(LiveId webId) const { return !out.webs[webId].segments.isEmpty(); }
    MachineLocation homeOfWeb(LiveId webId) const { return out.webs[webId].home(); }

    // Gives a web the location it will keep. One segment covering everything its interval does,
    // which is the whole of the persistent-location rule: a web is in one place wherever it is live.
    MachineLocation setHome(LiveId webId, RegisterClassId cls, MachineLocation location) {
        auto interval = webs[webId].interval();
        out.webs[webId].regClass = cls;

        out.webs[webId].segments.push(AllocationSegment {
            .from = interval.isEmpty() ? 0 : interval.first(),
            .to = interval.isEmpty() ? 0 : interval.last(),
            .location = location,
        });

        return location;
    }

    // Gives a split web its home and the location it steps into over each window, as the alternating
    // segment list the two describe. Windows are sorted, disjoint and strictly inside the interval -
    // planSplit builds them from clobber sites the web is live *across*, so there is always a live
    // point on either side of each - which is what makes every odd segment a home segment and the
    // first and last of them home segments in particular.
    template<class Windows>
    MachineLocation setSplit(LiveId webId, RegisterClassId cls, MachineLocation home,
        const Windows& windows, MachineLocation windowLocation)
    {
        auto interval = webs[webId].interval();
        assertTrue(!interval.isEmpty() && !windows.isEmpty());

        auto& segments = out.webs[webId].segments;
        out.webs[webId].regClass = cls;

        auto at = interval.first();

        for(auto& window: windows) {
            assertTrue(window.from > at && window.to < interval.last()); // strictly inside

            segments.push(AllocationSegment { .from = at, .to = window.from, .location = home });
            segments.push(AllocationSegment { .from = window.from, .to = window.to, .location = windowLocation });
            at = window.to;
        }

        segments.push(AllocationSegment { .from = at, .to = interval.last(), .location = home });
        return home;
    }

    // Whether these two webs are a pair no register may hold both of - see tieConflicts and
    // collectTieConflicts. The relation is symmetric, so one of the two lists answers it.
    bool tiesConflict(LiveId webId, LiveId other) const {
        for(auto id: tieConflicts[webId]) {
            if(id == other) return true;
        }

        return false;
    }

    // A register is available to a web when nothing already in it is ever live at the same time.
    // Comparing whole intervals rather than an endpoint against the walk's current position is what
    // lets a phi be placed at whichever predecessor edge reaches it first, and what lets a register
    // be reused across a web's holes.
    //
    // Disjoint lives are not quite the whole of it, which is what `tieConflicts` is here for: a
    // destructive result and a sibling operand of its instruction have disjoint lives *by
    // construction* - the operand's ends where the result's begins - and still may not share a
    // register, because the copy that puts operand zero into the result's register runs in front of
    // the instruction and would overwrite the sibling before it is read.
    //
    // Occupancy is tracked per *unit* rather than per register name, because two views of one
    // register are the same storage. Today every class covers its register completely and a unit is
    // a register, so this is one iteration; the loop is what stops that from being an assumption.
    //
    // What each occupant is compared against is its own claim rather than its life - see
    // RegisterClaim. For the whole-life claim the two are the same thing, which is what this always
    // was; for a cluster claim they are not, and the stretch is the answer.
    bool isFree(LiveId webId, RegisterClassId cls, PhysicalReg reg, const LiveInterval& interval) {
        auto units = targetRegisters().viewOf(cls, reg).units;

        for(Size i = 0; units; i++, units >>= 1) {
            if(!(units & 1)) continue;

            for(auto& claim: occupants[reg.bank][i]) {
                if(claimOf(claim).overlaps(interval)) return false;
                if(tiesConflict(webId, claim.web)) return false;
            }
        }

        return true;
    }

    // The stretch a claim holds its register over, as an interval the overlap test takes.
    LiveInterval claimOf(const RegisterClaim& claim) const {
        return claim.partial ? LiveInterval { &claim.stretch, 1 } : webs[claim.web].interval();
    }

    // A register a web has been given is held for the whole of that web's interval, windows
    // included. A split web is not *in* its register over a window, but the copies at either end of
    // one read and write it there, so it is not free to hold anything else either - and what could
    // fit is nothing: a window is one instruction wide, and the only value whose whole life fits
    // inside one is a result nothing reads.
    //
    // `occupyStretch` is the other kind of claim, and the one a window in a *register* rather than
    // in the frame needs: it hands out only the stretch, so the rest of the register's life is still
    // free for whoever else asks. See §5.9 - it is the only caller, and it runs once the walk below
    // has finished, so nothing here has to reason about a partial claim being displaced.
    void occupy(RegisterClassId cls, PhysicalReg reg, LiveId webId) {
        auto units = targetRegisters().viewOf(cls, reg).units;

        for(Size i = 0; units; i++, units >>= 1) {
            if(units & 1) occupants[reg.bank][i].push(RegisterClaim { .web = webId });
        }
    }

    void occupyStretch(RegisterClassId cls, PhysicalReg reg, LiveId webId, Range stretch) {
        auto units = targetRegisters().viewOf(cls, reg).units;

        for(Size i = 0; units; i++, units >>= 1) {
            if(units & 1) {
                occupants[reg.bank][i].push(RegisterClaim {
                    .web = webId, .stretch = stretch, .partial = true,
                });
            }
        }
    }

    // Places the web `v` belongs to, preferring `hint` so that a copy the encoder would otherwise
    // have to emit collapses into a no-op. A web that is already placed keeps what it has: every
    // value in it is one definition of a single quantity living in a single location.
    MachineLocation assign(LowerValue* v, RegSet extraAvoid, MachineLocation hint) {
        auto webId = webIdOf(v);
        auto& info = webs[webId];

        if(placed(webId)) return homeOfWeb(webId);

        auto cls = classForType(v->type);
        auto bank = targetRegisters().regClass(cls).bank;
        auto avoid = info.avoid | extraAvoid | displacedFrom[Size(webId)];
        auto interval = info.interval();

        /*
         * A previous pass found something that needed one of this web's registers more than it did.
         *
         * It goes homeless *here*, and is offered a register again only once the walk has finished -
         * see `reclaimDisplaced`. Letting it search now would undo the comparison that displaced it:
         * `findDisplacement` chose this web because it valued its register least of everything in
         * the way, and a web that answers that by taking the next register along has simply moved
         * the shortage to whoever asks after it. Which is measurable - on the program corpus,
         * searching here costs `Sieve` 15 ms and the corpus 21.
         *
         * What is left over at the end is a different question and a safe one: a register nothing
         * else took is one no comparison was ever made about.
         */
        if(!displacedFrom[Size(webId)].isEmpty()) {
            deferred.push(webId);
            return assignHomeless(webId, v->type, cls, interval);
        }

        /*
         * A value that costs nothing to recreate never takes a register, however free one looks
         * here.
         *
         * The search below asks whether a register is *available over this web's life*, and takes
         * one whenever it is. That question is the wrong one for a recipe: what a register buys such
         * a web is `homelessCost`, which is already known and is a *weighted* number - so a constant
         * read once on an abort arm is worth one cold instruction, and holding a register for it
         * denies that register to everything the web spans. §11.2 made that span the whole function:
         * an `imm 134` has no block of its own, so its interval runs from the entry to the last
         * abort arm, and Matrix's innermost loop was three registers short with `mov $0x86, %ebp`
         * sitting in one of them.
         *
         * The bar is one recreation at unit weight - a value read once, on a path that runs as often
         * as the function is entered. That is the abort arm exactly, and it is nothing else: a
         * constant read inside a loop carries that loop's weight and keeps its register, because
         * `rematCost` is weighted by where the reads *are* rather than by how many there are. Above
         * the bar the comparison is the one at the bottom of this function, where a register that is
         * not free is weighed against the recipe properly.
         */
        if(info.canRemat && info.rematCost <= kRematCost) {
            return assignHomeless(webId, v->type, cls, interval);
        }

        auto usable = [&](Size i, const RegSet& blocked) {
            auto reg = PhysicalReg { bank, U16(i) };
            if(!allocatable.has(reg) || blocked.has(reg)) return false;
            if(!targetRegisters().regClass(cls).allowedPhysical.has(reg)) return false;
            return isFree(webId, cls, reg, interval);
        };

        auto takes = [&](MachineLocation at) {
            return at.isPhysical() && at.bank == bank && usable(at.index, avoid);
        };

        // Two registers are worth having, and each is worth exactly the one copy taking it removes:
        // the caller's hint, which removes a copy standing at the instruction being placed, and the
        // web's own preference, which removes one standing where the web is read into a fixed
        // register (computeAvoidSets). So the two are compared by how often the copy each removes
        // would have run, and the preference takes a tie - the instruction defining a value writes
        // its result register whether or not the hint was taken, so the copy the preference removes
        // is the one more likely to have been unconditional.
        //
        // Neither is ever a compromise: `usable` is the same test the search below applies, so a
        // register taken this way is one first-fit could have handed out anyway.
        auto chosen = kNoRegister;
        auto hintOk = takes(hint);
        auto preferOk = takes(info.preferred);

        if(hintOk && preferOk) {
            chosen = info.preferredWeight >= currentWeight ? info.preferred.index : hint.index;
        } else if(hintOk) {
            chosen = hint.index;
        } else if(preferOk) {
            chosen = info.preferred.index;
        } else {
            for(Size i = 0; i < orderCount[bank]; i++) {
                if(usable(order[bank][i], avoid)) { chosen = order[bank][i]; break; }
            }
        }

        // No register is free for the whole of this web's life *and* safe across everything the web
        // outlives. Three ways out, and they are compared rather than tried in turn, because the
        // cheapest is a different one in each of the shapes that gets here:
        //
        //   - a window. Keep a register the clobbers made unusable, and step out of it across them.
        //     Costs a store and a reload where each clobber stands, and takes nothing from anything
        //     else: the registers it can use are exactly the ones nothing safe could have wanted.
        //   - a displacement. Take a register from an occupant that values it less, which costs
        //     whatever that occupant then pays. A safe register held by a value read once is worth
        //     far more to a value read at every iteration of a loop.
        //   - neither, and the web goes to the frame or to a recipe.
        if(chosen == kNoRegister) {
            SplitPlan plan;
            auto splitCost = planSplit(webId, cls, extraAvoid, interval, usable, plan)
                ? plan.cost + kSplitMargin
                : kNoCost;

            auto homeless = info.homelessCost();
            auto budget = splitCost < homeless ? splitCost : homeless;

            // The displaced web is not moved where it stands. This walk decides each web's location
            // in the order the instructions read them, and a web already placed has been offered to
            // everything that could have competed for its register, so taking it back now would
            // leave the earlier decisions inconsistent with the later ones. It is *recorded*
            // instead, and the next pass starts with it homeless, which leaves its register free at
            // the point that wanted it. Nothing has been emitted either way, which is what makes
            // another pass cheap.
            auto displaced = findDisplacement(webId, cls, avoid, interval, budget);
            if(displaced != kNoRegister) {
                recordDisplacement(webId, bank, displaced, interval);
                return assignHomeless(webId, v->type, cls, interval);
            }

            if(splitCost < homeless) return takeSplit(webId, cls, v->type, plan);
            return assignHomeless(webId, v->type, cls, interval);
        }

        auto reg = PhysicalReg { bank, U16(chosen) };
        occupy(cls, reg, webId);
        written.add(reg);

        // A cycle in a class with an exchange costs no register to break; in one without, it costs
        // a scratch register that has to have been held back before this pass ran. Two webs is where
        // that becomes possible, so it is where the reserve has to be measured.
        if(!classHasExchange(cls) && ++exchangeless[bank] == 2) requiresLegalizationTemps = true;

        return setHome(webId, cls, MachineLocation::physical(reg));
    }

    /*
     * Splitting.
     *
     * A web that is live across a call has to dodge everything the call destroys for the whole of
     * its life, which under most conventions leaves it the handful of preserved registers and
     * nothing else. Where those run out - and they run out at the first function that keeps more
     * than six things across a call - the web ends up in the frame and pays a reload at every one of
     * its reads, however far from the call they are.
     *
     * A split buys that back. The web is given a register it could have had if the call were not
     * there, and steps out of it into the frame over a *window*: the run of clobbering instructions
     * it crosses, from `afterInst(lo)` to `afterInst(hi) + 1`. It is still in the register when
     * instruction lo reads its operands, and back in it before instruction hi + 1 reads its own, so
     * the store joins lo's parallel copy and the reload joins hi+1's and no new emission slot
     * exists. That range is also why a window can never touch a block's entry or exit point - lo is
     * never a terminator, so the earliest a window starts is one point past a block entry and the
     * latest it ends is the terminator's own `before` - which is the boundary invariant WebAllocation
     * describes, and what keeps a location change out of the CFG.
     *
     * What it costs is a store and a reload per window, where the window stands. What it saves is
     * whatever the web would have paid for having no register - so a value read once next to the
     * call gains nothing and a value read a dozen times, or read inside a loop, gains a great deal.
     * kSplitMargin is what keeps the first case from being taken.
     */

    struct SplitPlan {
        Size reg = kNoRegister;

        // Sorted, disjoint, and strictly inside the web's interval. Inline, because one of these is
        // built per web the allocator cannot place outright; `bestWindows` next door is pooled for
        // the same reason from the other direction.
        //
        // Sixteen rather than the four this started at. The guess was "a web is split around one or
        // two calls", and the benchmark says otherwise: a web that survived the unsplit search is by
        // construction one that crosses a lot of clobbers, and four was over the line often enough
        // to be the largest allocation site in the backend.
        SmallArray<Range, 16> windows;

        U32 cost = 0; // what the windows cost, in computeSpillCosts' units
    };

    // Whether a window may cover this site at all. Two things say no. A terminator, because the copy
    // that brings the web home is emitted in the next instruction's parallel copy and a block that
    // has branched has no next instruction. And an instruction that defines one of the web's own
    // members, because then the value on the far side of the window is not the value that went into
    // it, and the reload would overwrite the definition with what the web held before.
    bool coverable(const ClobberSite& site, LiveId webId) const {
        if(site.terminator) return false;

        for(auto& created: site.inst->created()) {
            if(isImplicit(&created)) continue;

            auto id = created.liveId();
            if(id != kNullLive && out.webOf[id] == webId) return false;
        }

        return true;
    }

    // The windows `reg` needs if it is to hold this web: one per run of consecutive clobber sites
    // that write it. Consecutive sites become one window rather than two adjacent ones because two
    // would put a reload and a store in the same parallel copy, which is a cycle through the frame
    // where nothing has to move at all.
    void windowsFor(PhysicalReg reg, const Array<Size>& crossed, Array<Range>& windows, U32& cost) const {
        auto open = kNoRegister; // the site index the run under construction started at
        U32 last = 0;

        auto close = [&]() {
            auto& first = clobberSites[open];
            windows.push(Range { afterInst(first.index), afterInst(last) + 1 });
            cost += first.weight * (kStoreCost + kReloadCost);
        };

        for(auto i: crossed) {
            auto& site = clobberSites[i];
            if(!site.mask.has(reg)) continue;

            if(open != kNoRegister && site.index != last + 1) close();
            if(open == kNoRegister || site.index != last + 1) open = i;

            last = site.index;
        }

        if(open != kNoRegister) close();
    }

    // The cheapest split of this web, if there is one at all. `usable` is assign's own test, so the
    // register a split lands on is one the unsplit search would have accepted had the clobbers not
    // been in the way. Whether the price is worth paying is assign's question, not this one's.
    template<class Usable>
    bool planSplit(LiveId webId, RegisterClassId cls, const RegSet& extraAvoid,
        const LiveInterval& interval, Usable&& usable, SplitPlan& out_)
    {
        auto& info = webs[webId];
        auto bank = targetRegisters().regClass(cls).bank;

        // Every clobber site the web outlives, and the registers written by the ones no window may
        // cover - which are simply more registers the web has to avoid, exactly as before.
        auto& crossed = scratch.crossed;
        crossed.clear();

        auto blocked = info.avoidFixed | extraAvoid;

        for(Size i = 0; i < clobberSites.size(); i++) {
            if(!interval.crosses(clobberSites[i].index)) continue;

            if(coverable(clobberSites[i], webId)) crossed.push(i);
            else blocked |= clobberSites[i].mask;
        }

        if(crossed.isEmpty()) return false;

        auto best = kNoRegister;
        U32 bestCost = 0;
        // The windows of the register being priced and of the cheapest one so far. Two buffers
        // rather than one per candidate: this runs the whole allocation order for every web that
        // could not simply be given a register, so a homeless web in a large function used to cost
        // an allocation per register in its bank.
        auto& windows = scratch.windows;
        auto& bestWindows = scratch.bestWindows;
        bestWindows.clear();

        for(Size k = 0; k < orderCount[bank]; k++) {
            auto i = order[bank][k];
            if(!usable(i, blocked)) continue;

            windows.clear();
            U32 cost = 0;
            windowsFor(PhysicalReg { bank, U16(i) }, crossed, windows, cost);

            // A register needing no window is one the unsplit search would already have taken.
            assertTrue(!windows.isEmpty());

            if(best == kNoRegister || cost < bestCost) {
                best = i;
                bestCost = cost;

                bestWindows.clear();
                for(auto& window: windows) bestWindows.push(window);
            }
        }

        if(best == kNoRegister) return false;

        out_.reg = best;
        out_.cost = bestCost;

        out_.windows.clear();
        for(auto& window: bestWindows) out_.windows.push(window);
        return true;
    }

    // Carries out a plan: the register for the whole interval, and one frame slot for every window.
    // One slot rather than one per window, since the windows of a single web never overlap and the
    // value in them is the same value.
    MachineLocation takeSplit(LiveId webId, RegisterClassId cls, LowerType type, SplitPlan& plan) {
        auto bank = targetRegisters().regClass(cls).bank;
        auto reg = PhysicalReg { bank, U16(plan.reg) };

        occupy(cls, reg, webId);
        written.add(reg);
        if(!classHasExchange(cls) && ++exchangeless[bank] == 2) requiresLegalizationTemps = true;

        // A window wider than one instruction contains a `before` point, so something may read the
        // web out of the slot there and need a scratch register to do it. A one-instruction window
        // contains none, and asks for nothing.
        for(auto& window: plan.windows) {
            if(window.to > window.from + 1) requiresLegalizationTemps = true;
        }

        auto slot = takeSlot(webId, stackSlotClassFor(type), intervalOf(plan.windows));
        return setSplit(webId, cls, MachineLocation::physical(reg), plan.windows, MachineLocation::stack(slot));
    }

    // Whether this occupant would have to go for `webId` to take the register it is in. Every
    // occupant whose life overlaps, and - for the same reason isFree refuses one - every occupant
    // the web ties against, whose life does not overlap and which would still be read after the
    // copy in front of its instruction had overwritten it.
    bool displaces(LiveId webId, const RegisterClaim& claim, const LiveInterval& interval) const {
        // Cluster claims are handed out after the walk that displaces anything has finished, so
        // there is no such thing as displacing one and nothing here has to price it.
        assertTrue(!claim.partial);
        return webs[claim.web].interval().overlaps(interval) || tiesConflict(webId, claim.web);
    }

    // The register whose occupants would be cheapest to displace, if displacing them costs less than
    // `budget` - what the web asking for a register would otherwise pay. The register is only free
    // for the new web once all of them have gone, so the price is all of theirs together.
    Size findDisplacement(LiveId webId, RegisterClassId cls, const RegSet& avoid,
        const LiveInterval& interval, U32 budget) const
    {
        auto bank = targetRegisters().regClass(cls).bank;

        auto best = kNoRegister;
        auto bestCost = budget;

        for(Size k = 0; k < orderCount[bank]; k++) {
            auto i = order[bank][k];
            auto reg = PhysicalReg { bank, U16(i) };
            if(!allocatable.has(reg) || avoid.has(reg)) continue;
            if(!targetRegisters().regClass(cls).allowedPhysical.has(reg)) continue;

            U32 cost = 0;
            for(auto& claim: occupants[bank][i]) {
                if(displaces(webId, claim, interval)) cost += webs[claim.web].homelessCost();
            }

            if(cost < bestCost) { bestCost = cost; best = i; }
        }

        return best;
    }

    void recordDisplacement(LiveId webId, RegisterBankId bank, Size reg, const LiveInterval& interval) {
        auto physical = PhysicalReg { bank, U16(reg) };

        for(auto& claim: occupants[bank][reg]) {
            if(displaces(webId, claim, interval)) {
                displacementRequests.push(Placement::DisplacementRequest { .web = claim.web, .reg = physical });
            }
        }
    }

    // A web that is not getting a register. There are two ways to do without one and they cost
    // differently: recreating the value wherever it is read, or keeping it in the frame and bringing
    // it back at each instruction that cannot read a memory operand. computeSpillCosts priced both.
    MachineLocation assignHomeless(LiveId webId, LowerType type, RegisterClassId cls, const LiveInterval& interval) {
        auto& info = webs[webId];
        requiresLegalizationTemps = true;

        if(info.canRemat && info.rematCost < info.spillCost) {
            remats.push(info.recipe);
            return setHome(webId, cls, MachineLocation::remat(RematId(remats.size() - 1)));
        }

        auto slot = takeSlot(webId, stackSlotClassFor(type), interval);
        return setHome(webId, cls, MachineLocation::stack(slot));
    }

    // Whether anything already promised this slot is a web that may not share a location with
    // `webId` - the destructive-encoding tie of isFree, asked of a frame slot. It is the same
    // question and it has to be asked in the same two places, because a tie is a statement about two
    // webs sharing *a location* and not about their sharing a register: the copy that puts operand
    // zero where the result goes runs in front of the instruction whether that place is a register
    // or a slot, and overwrites a sibling operand sitting there either way.
    bool slotTiesConflict(StackSlotId slot, LiveId webId) const {
        for(auto id: slotWebs[slot]) {
            if(tiesConflict(webId, id)) return true;
        }

        return false;
    }

    // A slot in the frame for the stretches `ranges` covers, reusing one nothing is using over any of
    // them. Slots are recycled by exactly the rule registers are, so the frame ends up as large as
    // the peak number of simultaneously spilled webs rather than as large as their total - and a
    // split web's window, being a stretch and not a life, shares slots with whatever is dead there.
    //
    // "Exactly the rule registers are" includes the tie above, which is the half of that rule that
    // interval overlap does not state: a tied result's life begins where its sibling operand's ends,
    // so the two are always offered each other's storage and the ranges never object.
    StackSlotId takeSlot(LiveId webId, StackSlotClass slotClass, const LiveInterval& ranges) {
        while(slotOccupants.size() < frame.slots.size()) slotOccupants.push();
        while(slotWebs.size() < frame.slots.size()) slotWebs.push();

        auto claim = [&](StackSlotId slot) {
            mergeRanges(slotOccupants[slot], ranges.ranges, ranges.count, scratch.merged);
            slotWebs[slot].push(webId);
            return slot;
        };

        for(Size i = 0; i < frame.slots.size(); i++) {
            if(frame.slots[i].kind != StackSlotKind::Spill) continue;
            if(frame.slots[i].slotClass != slotClass) continue;
            if(intervalOf(slotOccupants[i]).overlaps(ranges)) continue;
            if(slotTiesConflict(StackSlotId(i), webId)) continue;

            return claim(StackSlotId(i));
        }

        auto size = stackSlotSize(slotClass);
        auto slot = frame.add(StackSlot {
            .kind = StackSlotKind::Spill,
            .slotClass = slotClass,
            .size = size,
            .alignment = size,
        });

        while(slotOccupants.size() <= slot) slotOccupants.push();
        while(slotWebs.size() <= slot) slotWebs.push();
        return claim(slot);
    }
};

/*
 * Pass 0: build the webs.
 */

// The pairs of values that have to be kept apart for a reason interval overlap does not state - see
// buildWebs and Placer::isFree. Indexed by web representative while the webs are being built, and
// merged onto the representative exactly as the ranges are, so a web carries everything merged into
// it; rewritten in terms of web ids once they are, since that is what everything afterwards asks in.
using TieConflicts = ArrayList<LiveId>;

// A destructive encoding's result and the *other* operands of its instruction. The result is written
// over operand zero by a copy emitted in front of the instruction, so any sibling operand sharing the
// result's location is read after that copy has already overwritten it.
//
// Their intervals never overlap, which is exactly what a tie means: the operand's life ends at
// `beforeInst` and the result's begins at `afterInst`. So the interference test below sees two webs
// that are perfectly mergeable, and a loop that swaps two values asks for that merge at every latch -
// the phi takes its incoming value from an operation whose other operand is the phi itself.
//
// The same disjointness makes the two look like ideal *neighbours in one register*, which is the
// other half of what this is for. Refusing only the merge is not enough: two webs that were never
// merged are still offered the same register by first-fit, and which of them notices depends on which
// is placed first - the result is placed at its instruction, but a result that shares a web with a phi
// is placed at whichever predecessor edge reached that phi, which can be long before. So placement
// treats a pair here as interference (Placer::isFree) rather than only as an unmergeable pair.
//
// Operand zero is deliberately not here, since the result sharing its location is the whole content
// of the tie; and neither is an operand that *is* operand zero (`sub %a, %a`), where the copy in
// front of the instruction is an identity and overwrites nothing.
static void collectTieConflicts(Placer& a, TieConflicts& out) {
    out.reset(a.out.webOf.size());

    auto onInst = [&](LowerInst* inst) {
        if(a.machine.formOf(inst).tiedResult() != 0) return;
        if(inst->createdCount == 0 || isImplicit(&inst->created()[0])) return;

        auto used = inst->used();
        if(used.size() == 0) return;

        auto result = inst->created()[0].liveId();

        auto conflicts = [&](LowerValue* value) {
            if(isImplicit(value)) return;

            out[result].push(value->liveId());
            out[value->liveId()].push(result);
        };

        for(Size i = 1; i < used.size(); i++) {
            if(used[i] == used[0]) continue;
            conflicts(a.base[used[i]]);
        }

        // A folded address is read after that copy too, and its registers are not operands of this
        // instruction at all: they belong to the X86Address one above it, whose own life ends there.
        // So nothing else would keep the result off them - the address value itself is implicit, and
        // the interference test sees two lives that do not overlap - and the copy in front would
        // then compute the result into the very register the address is about to be read through.
        //
        // Reachable since a load can be folded into a destructive operation (foldLoads in
        // transform.cpp); before that, the only instructions with a folded address were the ones
        // with no copy in front of them.
        auto address = a.machine.formOf(inst).addressOperand();
        if(address >= 0 && isMem(a.base[used[address]])) {
            for(auto part: a.base[used[address]]->inst()->used()) conflicts(a.base[part]);
        }
    };

    for(auto offset: a.fun.blocks.contents(a.base)) {
        auto block = a.base[offset];
        for(auto i: block->instructions.contents(a.base)) onInst(a.base[i]);
        onInst(a.base[block->terminator]);
    }
}

/*
 * Whether this instruction is a plain copy: one value in, the same number out, and an encoding that
 * writes no bytes at all once the two are in one place.
 *
 * Asked of the *form* rather than of the instruction's kind, because that is where the property
 * lives. `omitWhenSame` is the encoder's statement that source and destination in one register make
 * this instruction nothing - a bitcast between two integer classes, a cast the peephole proved
 * changes no bit, a move - and a form that has to emit something anyway is one whose two ends are
 * not the same number: `FormCastMov` clears the upper half of its destination, and that clearing is
 * the whole reason it exists.
 *
 * The class has to match at both ends as well. Two views of one register file are the same storage,
 * but a web is placed and copied as one class, and `verifyPlacement` checks exactly that.
 */
static bool isRegisterCopy(Placer& a, LowerInst* inst) {
    if(!a.machine.formOf(inst).encoding.omitWhenSame) return false;
    if(inst->createdCount != 1 || inst->usedCount != 1) return false;

    auto& result = inst->created()[0];
    auto source = a.base[inst->used()[0]];
    if(isImplicit(&result) || isImplicit(source)) return false;

    return classForType(result.type) == classForType(source->type);
}

static void buildWebs(Placer& a) {
    // Union-find over values, with the web's merged interval kept on the representative so that the
    // interference test is against everything already merged into it rather than against one member.
    auto& parent = a.scratch.parent;
    parent.clear();
    for(Size i = 0; i < a.out.webOf.size(); i++) parent.push(LiveId(i));

    auto find = [&](LiveId id) {
        while(parent[id] != id) {
            parent[id] = parent[parent[id]]; // halve the path as we go
            id = parent[id];
        }

        return id;
    };

    auto& tieConflicts = a.scratch.valueTies;
    collectTieConflicts(a, tieConflicts);

    auto tiesConflict = [&](LiveId left, LiveId right) {
        for(auto id: tieConflicts[left]) {
            if(find(id) == right) return true;
        }

        for(auto id: tieConflicts[right]) {
            if(find(id) == left) return true;
        }

        return false;
    };

    // One web's ranges and conflicts taken over by another. Both phases below do exactly this and
    // differ only in what they ask first.
    auto merge = [&](LiveId left, LiveId right) {
        mergeRanges(a.webs[left].ranges, a.webs[right].ranges.pointer(), a.webs[right].ranges.size(),
            a.scratch.merged);
        a.webs[right].ranges.clear();

        for(auto id: tieConflicts[right]) tieConflicts[left].push(id);
        tieConflicts[right].clear();

        parent[right] = left;
    };

    // Hottest block first, because a merge can fail: two phis may each want to join a web, and only
    // the first of them to ask can, since the second then overlaps what the first merged in. The one
    // in the hotter block is the copy worth removing - a move in a loop body costs a multiple of the
    // same move outside it - so it is the one that gets to ask first.
    //
    // Blocks of equal frequency keep their list order, which is what keeps the result reproducible
    // in the goldens: the sort below is a stable insertion sort over the block list.
    auto blockList = a.fun.blocks.contents(a.base);
    auto weightAt = [&](Size position) {
        return a.frequency.frequencyOf(a.base[blockList[position]]->index);
    };

    auto& blockOrder = a.scratch.blockOrder;
    blockOrder.clear();
    for(Size i = 0; i < blockList.size(); i++) blockOrder.push(i);

    for(Size i = 1; i < blockOrder.size(); i++) {
        auto v = blockOrder[i];
        auto weight = weightAt(v);
        auto j = i;

        while(j > 0 && weightAt(blockOrder[j - 1]) < weight) {
            blockOrder[j] = blockOrder[j - 1];
            j--;
        }

        blockOrder[j] = v;
    }

    /*
     * The copies, first, and with no interference test at all - which is the whole of the
     * difference between this phase and the one below it.
     *
     * A copy's two ends hold *the same number*. Whether their lives overlap is therefore not a
     * question about whether one location can serve both: it can, for the same reason `omitWhenSame`
     * exists, and the copy then emits nothing. The interval test the phi merges make is what a web
     * needs when its members are different quantities that merely happen not to coexist, and asking
     * it of a copy is what left `mov rdx, rcx` standing wherever `rcx` was read again afterwards -
     * §16's ninth list, thirteen of them in `Sort.sort` alone.
     *
     * Doing them all before any phi merge is what keeps that sound, and it is not a preference. The
     * relation "holds the same number" is transitive over copy edges and nothing else: a web built
     * out of copies alone may have any number of its members live at once, and a phi merge on top of
     * it still asks that the *whole* web be dead wherever the phi's web is live. Interleaving the
     * two would let a copy merge put a value alongside a phi that was admitted only because the
     * ranges were disjoint, and those two are not one number.
     */
    for(auto position: blockOrder) {
        auto block = a.base[blockList[position]];

        for(auto i: block->instructions.contents(a.base)) {
            auto inst = a.base[i];
            if(!isRegisterCopy(a, inst)) continue;

            auto left = find(inst->created()[0].liveId());
            auto right = find(a.base[inst->used()[0]]->liveId());
            if(left == right) continue;

            // The one thing that still has to be asked, and for the reason it always did: a
            // destructive result and a sibling operand of its instruction are read and written in an
            // order the intervals do not describe, so being the same number would not save them.
            if(tiesConflict(left, right)) continue;

            merge(left, right);
        }
    }

    // Which values a copy has proved equal, before the phi merges below can widen a web past that.
    // Placement itself never asks - a web is a web - but the verifier does: two overlapping values
    // in one location are a mistake unless this says they are one number. See verifyPlacement.
    for(Size i = 0; i < a.out.webOf.size(); i++) a.out.copyClassOf.push(find(LiveId(i)));

    for(auto position: blockOrder) {
        auto block = a.base[blockList[position]];

        for(auto p: block->phis.contents(a.base)) {
            auto phi = a.base[p];
            auto& result = phi->result;
            if(isImplicit(&result)) continue;

            for(auto u: phi->used()) {
                auto value = a.base[u];
                if(isImplicit(value)) continue;

                auto left = find(result.liveId());
                auto right = find(value->liveId());
                if(left == right) continue;

                // The one thing that has to hold: a phi and an incoming value are two quantities
                // rather than one, so no member of either web may be live where a member of the
                // other is.
                if(a.webs[left].interval().overlaps(a.webs[right].interval())) continue;

                // And the one thing that does not follow from it - see collectTieConflicts.
                if(tiesConflict(left, right)) continue;

                merge(left, right);
            }
        }
    }

    // Flatten, so that everything afterwards is a direct lookup rather than a find().
    for(Size i = 0; i < a.out.webOf.size(); i++) a.out.webOf[i] = find(LiveId(i));

    // And restate the conflicts in the same terms, since placement asks them of webs rather than of
    // values. One direction is enough: collectTieConflicts records both, and a merge moves a web's
    // entries onto the representative without dropping the ones pointing back at it.
    for(Size i = 0; i < tieConflicts.size(); i++) {
        for(auto id: tieConflicts[i]) a.tieConflicts[LiveId(i)].push(a.out.webOf[id]);
    }
}

/*
 * Pass 1: work out which registers each value has to stay out of - and, from the same walk, the one
 * register each web would rather be in.
 *
 * The two come out of the same question asked in opposite directions. A fixed-register operand is a
 * register the web must not be *left* in over the instruction that writes it, and a register the web
 * has to be *in* at the instruction that reads it - so the same site that adds to an avoid set adds
 * to somebody else's preference, and a web that ends in `ret` is told here that rax is where it
 * wants to live rather than discovering it one copy too late.
 */

static void computeAvoidSets(Placer& a) {
    U32 index = 0;

    // One entry per instruction, so that §5.9 can ask what a run of them writes without searching.
    auto& siteOfInst = a.scratch.siteOfInst;
    siteOfInst.clear();
    for(U32 i = 0; i < a.live.instCount; i++) siteOfInst.push(kNoSite);

    for(auto offset: a.fun.blocks.contents(a.base)) {
        auto block = a.base[offset];
        auto weight = a.weightOf(block);

        auto onInst = [&](LowerInst* inst, bool terminator) {
            Scratch<InstShape> held(a.shapes);
            auto& shape = *held;
            shapeOf(a.base, a.machine, a.constraints, a.fun, inst, shape);
            auto mask = writtenRegisters(shape);
            a.written |= mask;

            if(!mask.isEmpty()) {
                /*
                 * An operand that the parallel copy in front of this instruction does *not* place
                 * is read straight out of its own register, so that register has to survive both
                 * the copy and whatever the instruction's expansion writes before reading its
                 * sources (`xor rdx, rdx` ahead of a division, r11 as scratch in an unrolled copy).
                 *
                 * This half is `avoidFixed` as well: it is the operand's own instruction, so there
                 * is no window that could carry the web past it - it has to be readable *here*.
                 *
                 * A **call** is the exception, and it is the one that pays. Its clobber set is the
                 * callee's, and the callee has not run when the target operand is read - so the only
                 * registers that operand has to dodge are the fixed ones the copy in front of it
                 * writes. Charging it the whole clobber set left an indirect call's target with the
                 * preserved registers and nothing else, which is exactly the set the values living
                 * *across* the call are competing for: `each` in `Pipeline.yana` spent rbp on a
                 * function pointer loaded one instruction earlier and reloaded its item pointer from
                 * the frame every iteration for want of it.
                 */
                auto operandMask = shape.isCall ? fixedRegisters(shape) : mask;

                auto used = inst->used();
                for(Size i = 0; i < used.size(); i++) {
                    auto v = a.base[used[i]];
                    if(isImplicit(v)) continue;
                    if(shape.uses[i].kind != ArgLocation::None) continue;

                    auto& web = a.webFor(v);
                    web.avoid |= operandMask;
                    web.avoidFixed |= operandMask;
                }

                // A return ends the function, so nothing can be live across it.
                if(!shape.isReturn) {
                    siteOfInst[index] = U32(a.clobberSites.size());
                    a.clobberSites.push(ClobberSite {
                        .index = index,
                        .mask = mask,
                        .operandMask = operandMask,
                        .inst = inst,
                        .weight = weight,
                        .terminator = terminator,
                    });
                }
            }

            // And the preference, from the operands the loop above skipped: the ones the parallel
            // copy *does* place, which is precisely where a copy is emitted unless the web is
            // already there. Outside the mask test, because a return writes nothing and is the site
            // this exists for.
            {
                auto used = inst->used();
                for(Size i = 0; i < used.size(); i++) {
                    auto want = wantForUse(shape, i);
                    if(!want.isPhysical()) continue;

                    auto v = a.base[used[i]];
                    if(isImplicit(v)) continue;

                    // The first site of a given weight keeps it, so a preference is decided by how
                    // often it runs and then by where it is - and never by how the walk happens to
                    // reach two sites that are worth exactly the same.
                    auto& web = a.webFor(v);
                    if(weight <= web.preferredWeight) continue;

                    web.preferred = want;
                    web.preferredWeight = weight;
                }
            }

            index++;
        };

        for(auto i: block->instructions.contents(a.base)) onInst(a.base[i], false);
        onInst(a.base[block->terminator], true);
    }

    // The other half: a clobber a web merely outlives. This is the part a split can buy back, and
    // the only part - which is why it lands in `avoid` alone and not in `avoidFixed`.
    for(Size i = 0; i < a.webs.size(); i++) {
        auto& web = a.webs[i];
        auto interval = web.interval();
        if(interval.isEmpty()) continue;

        for(auto& site: a.clobberSites) {
            if(interval.crosses(site.index)) web.avoid |= site.mask;
        }
    }
}

/*
 * Pass 1b: what each web costs if it does not get a register.
 *
 * Two prices rather than one, because there are two ways to do without a register and they are not
 * alike. A web in the frame pays a store where it is defined and a reload at every instruction that
 * has no form reading a memory operand - but nothing at all where such a form exists, since
 * `add rax, [slot]` is one instruction just as `add rax, rcx` is. A rematerialized web pays a
 * materialization at every read and nothing anywhere else: no store, no slot, and no interference
 * with anything, since it is not live between its uses.
 *
 * Both are stated in quarter-instructions, so that "a longer encoding" can be told apart from
 * "free", and both are weighted by how often the block they are in actually runs - see the cost
 * constants and Placer::weightOf above, which planSplit prices a window against.
 */

// Whether this instruction reads and writes its read/write memory operand in the same place - which it
// does when the operand and the result turn out to be one web, so that they are certain to share
// whatever home that web gets. This is the shape phi-web coalescing produces for a loop-carried
// accumulator, and the one case where a spilled result costs nothing to store.
//
// Asked of the *webs* rather than of their locations, because this runs before anything has been
// placed. `takesInPlace` is the same question once there are locations to compare, and the two have to
// agree for the costing to have priced what legalization will actually do.
static bool isInPlace(Placer& a, LowerInst* inst, const DirectMemoryChoice& choice) {
    if(!choice.hasReadWrite() || inst->createdCount == 0) return false;

    auto& result = inst->created()[0];
    auto operand = a.base[inst->used()[choice.readWrite]];

    if(isImplicit(&result) || isImplicit(operand)) return false;
    return a.sameWeb(operand, &result);
}

// The recipe that recreates `v`, if it is cheap and reproducible enough to have one. Every kind here
// is a constant in the sense that matters: it depends on nothing the program can write, so it
// produces the same answer wherever it is placed.
static bool recipeFor(Placer& a, LowerValue* v, Remat& out) {
    auto inst = v->inst();

    switch(inst->kind) {
        case LowerInst::Imm:
            // A float immediate that survived pooling is one `poolFloatConstants` decided to leave
            // alone, and recreating it is a general register and a bank crossing rather than the one
            // `mov` a recipe is priced at. The pooled ones arrive below as a Load instead.
            if(!isIntLike(v->type)) return false;

            out = Remat { .kind = Remat::Immediate, .type = v->type, .imm = ((LowerImm*)inst)->i };
            return true;

        case LowerInst::Load: {
            /*
             * The one load that reproduces: the contents of a global nothing writes.
             *
             * `mut` clear is a promise rather than a hint - it is what becomes LLVM's `constant` -
             * so no store the program makes can change what this answers, and the load gives the
             * same value wherever it is placed. That is exactly what a recipe requires: a pooled
             * `movsd xmm, [rip + k]` costs the same eight bytes where it is read as it did where it
             * was defined, against a spill's store plus its reload.
             *
             * The address has to *be* the global rather than be computed from it - `[rip + g]` has
             * no base or index field, so anything else would need a register the recipe has no way
             * to produce. And the load has to be the whole of the value: a narrower one extends,
             * and the recipe emits a plain move of the value's own width.
             */
            auto load = (LowerInstLoad*)inst;
            auto address = a.base[load->from];
            if(address->inst()->kind != LowerInst::Global) return false;

            auto global = a.base[((LowerInstGlobal*)address->inst())->target];
            if(global->mut) return false;
            if(load->getWidth() != accessWidthOf(v->type)) return false;

            out = Remat { .kind = Remat::ConstantLoad, .type = v->type };
            out.global = global;
            return true;
        }

        case LowerInst::Global:
            out = Remat { .kind = Remat::GlobalAddress, .type = v->type };
            out.global = a.base[((LowerInstGlobal*)inst)->target];
            return true;

        case LowerInst::Fun:
            out = Remat { .kind = Remat::FunctionAddress, .type = v->type };
            out.function = a.base[((LowerInstFun*)inst)->target];
            return true;

        case LowerInst::Alloca: {
            // Only a fixed-size one. A dynamic allocation moves the stack pointer, so its address is
            // not a constant offset from anything, and running it again would hand out fresh memory
            // rather than reproduce the same pointer.
            auto ref = a.frame.references.getValue(inst);
            if(!ref) return false;

            out = Remat { .kind = Remat::FrameAddress, .type = v->type };
            out.frame = ref.unwrap();
            return true;
        }

        default:
            return false;
    }
}

static void computeSpillCosts(Placer& a) {
    // A recipe reproduces one definition, so only a web that has one can have a recipe. A web with
    // several members is several definitions of a single location, and no one of them describes it.
    auto& members = a.scratch.members;
    members.clear();
    for(Size i = 0; i < a.webs.size(); i++) members.push(0);
    for(auto web: a.out.webOf) members[web]++;

    for(Size i = 0; i < a.out.webOf.size(); i++) {
        auto& info = a.webs[a.out.webOf[i]];
        if(members[a.out.webOf[i]] != 1 || info.canRemat) continue;

        auto v = a.live.getValue(LiveId(i));
        if(isImplicit(v)) continue;

        info.canRemat = recipeFor(a, v, info.recipe);
    }

    for(auto offset: a.fun.blocks.contents(a.base)) {
        auto block = a.base[offset];
        auto weight = a.weightOf(block);

        auto onInst = [&](LowerInst* inst) {
            Scratch<InstShape> held(a.shapes);
            auto& shape = *held;
            shapeOf(a.base, a.machine, a.constraints, a.fun, inst, shape);

            // The one operand this instruction could leave in the frame, if any. A read-modify-write
            // form takes precedence: it makes both the read and the write free, where a memory
            // source only makes the read free, and the two want the same r/m field.
            auto choice = directMemoryOperands(a.base, a.machine, inst);
            auto inPlace = isInPlace(a, inst, choice);
            auto folded = inPlace ? choice.readWrite : choice.read;

            auto used = inst->used();
            for(Size i = 0; i < used.size(); i++) {
                auto v = a.base[used[i]];
                if(isImplicit(v)) continue;

                // An operand the encoding pins to a particular register is copied into it from
                // wherever it lives, and that copy exists whether the home is a register, a slot or
                // a recipe. Being homeless costs such an operand a longer encoding rather than an
                // instruction, which is the same thing a memory operand costs.
                auto constrained = shape.uses[i].kind == ArgLocation::Register;
                auto free = constrained || I32(i) == folded;

                auto& web = a.webFor(v);
                web.spillCost += weight * (free ? kFoldedUseCost : kReloadCost);
                web.rematCost += weight * (constrained ? kFoldedUseCost : kRematCost);
            }

            auto created = inst->created();
            for(Size i = 0; i < created.size(); i++) {
                if(isImplicit(&created[i])) continue;

                // Written in place, so there is nothing to carry anywhere; or produced in a fixed
                // register and carried out of it either way, in which case the carrying move simply
                // becomes a store.
                if(inPlace && i == 0) continue;

                auto constrained = shape.creates[i].kind == ArgLocation::Register;
                a.webFor(&created[i]).spillCost += weight * (constrained ? kFoldedUseCost : kStoreCost);
            }
        };

        for(auto i: block->instructions.contents(a.base)) onInst(a.base[i]);
        onInst(a.base[block->terminator]);
    }
}

/*
 * Pass 2: place every web.
 *
 * One walk of the function in the order legalization will later read it, so that a web is placed at
 * the first instruction that has anything to say about where it should go. What each instruction has
 * to say is a hint and, for a destructive encoding, a set of registers to keep away from; nothing
 * here decides where an operand is *read*, which is legalization's question.
 */

// The register a freshly defined value would rather have: the one its source operand is about to
// vacate, so that the copy legalization would otherwise emit becomes `mov r, r` and disappears.
static MachineLocation copyHint(Placer& a, LowerInst* inst, U32 index) {
    auto used = inst->used();
    if(used.size() == 0) return MachineLocation::invalid();

    auto source = a.base[used[0]];
    if(isImplicit(source)) return MachineLocation::invalid();

    auto webId = a.webIdOf(source);
    auto interval = a.webs[webId].interval();
    if(!a.placed(webId) || interval.isEmpty()) return MachineLocation::invalid();

    // Only if the operand is genuinely finished with the register here: a value still live
    // after this instruction cannot hand its register to the result.
    if(interval.last() > afterInst(index)) return MachineLocation::invalid();

    return a.homeOfWeb(webId);
}

static void placeInst(Placer& a, LowerInst* inst, U32 index) {
    Scratch<InstShape> held(a.shapes);
    auto& shape = *held;
    shapeOf(a.base, a.machine, a.constraints, a.fun, inst, shape);
    auto used = inst->used();
    auto created = inst->created();

    // The form states which operand the result is written over, if any. Every one described so far
    // ties to operand zero, which is what legalization assumes when it copies that operand into the
    // result's register; a form tying to any other would need that copy to move.
    auto tied = a.machine.formOf(inst).tiedResult();
    assertTrue(tied <= 0); // a result tied to an operand other than the first

    bool tiedPlaced = false;

    if(tied == 0 && used.size() > 0 && created.size() > 0 && !isImplicit(&created[0])) {
        // A destructive result is where used()[0] must sit by the time the instruction runs, so it
        // has to avoid wherever the *other* operands are read from: the copy that puts used()[0]
        // there runs before the instruction, and would otherwise overwrite a sibling operand the
        // instruction has not read yet. Where those are read from is legalization's rule, asked
        // here of the placement so far.
        RegSet blocked;
        for(Size i = 1; i < used.size(); i++) {
            auto site = useSiteOf(a.base, a.machine, a.out, inst, shape, i,
                index, MachineLocation::invalid(), false);

            // Nothing has been handed out yet at this point in the instruction, so an operand that
            // needs a scratch register would get the first of them.
            if(site.needsTemp) blocked.add(a.temporaries.operandTemp(site.tempBank, 0));
            else if(site.at.isPhysical()) blocked.add(site.at.physicalReg());
        }

        a.assign(&created[0], blocked, copyHint(a, inst, index));
        tiedPlaced = true;
    }

    for(Size i = 0; i < created.size(); i++) {
        auto& v = created[i];
        if(isImplicit(&v)) continue;
        if(i == 0 && tiedPlaced) continue;

        auto want = wantForResult(shape, i);
        a.assign(&v, RegSet {}, want.isValid() ? want : copyHint(a, inst, index));
    }
}

// Places any phi in `successor` that hasn't been reached yet. The first predecessor edge to reach a
// phi decides its location, preferring the one the value arriving over that edge is vacating - which
// collapses the copy legalization would otherwise emit. `assign` only takes a hint when the register
// is genuinely free for the phi's whole interval, so offering one is always safe.
static void placePhis(Placer& a, LowerBlock* block, LowerBlock* successor) {
    for(auto p: successor->phis.contents(a.base)) {
        auto phi = a.base[p];
        auto& result = phi->result;
        if(isImplicit(&result)) continue;

        auto sources = phi->sources();
        auto incoming = phi->used();
        LowerValue* value = nullptr;

        for(Size i = 0; i < sources.size(); i++) {
            if(a.base[sources[i]] == block) { value = a.base[incoming[i]]; break; }
        }

        // Not an edge this phi takes a value from.
        if(!value || isImplicit(value)) continue;

        // Coalesced: the two are one web and one location, so there is nothing to hint at and the
        // transfer is an identity. The web is placed by whichever of its members is reached first.
        if(a.sameWeb(value, &result)) {
            a.assign(&result, RegSet {}, MachineLocation::invalid());
            continue;
        }

        a.assign(&result, RegSet {}, a.homeOfWeb(a.webIdOf(value)));
    }
}

// Places the incoming arguments and records where each arrived, so that legalization can emit the
// copies that move them out of the places the calling convention delivered them in. An argument that
// outlives a call can't stay in a register the call clobbers, so it is given a safe one and copied
// there on entry - once, rather than being shuffled at every call site.
//
// Where each argument arrived is asked of the convention, not worked out here: the caller placed
// them from the same answer, so there is no second rule that could drift out of step with it.
static void placeArgs(Placer& a, const CallConvention& convention) {
    auto args = a.fun.args.contents(a.base);

    ArgLocationList locations;
    classifyArgs(convention, args.size(), [&](Size i) {
        return a.base[args[i]]->result.type;
    }, locations);

    for(Size i = 0; i < args.size(); i++) {
        auto& result = a.base[args[i]]->result;

        if(isImplicit(&result)) {
            a.incomingArgs.push(MachineLocation::invalid());
            continue;
        }

        auto incoming = MachineLocation::physical(locations[i].reg);

        if(locations[i].kind == ArgLocation::Stack) {
            // The caller left this one in the argument area, at the offset the convention gave it -
            // an address that belongs to the caller's frame, not one this frame may choose. The
            // object is recorded even for an argument nothing reads, because it is part of the
            // frame the caller built whether this function looks at it or not.
            auto slot = a.frame.add(StackSlot {
                .kind = StackSlotKind::IncomingArg,
                .slotClass = StackSlotClass::Slot64,
                .size = 8,
                .alignment = 8,
                .argOffset = locations[i].stackOffset,
            });

            // Loaded into a register once on entry and read from there afterwards, exactly like a
            // register argument that had to be moved somewhere safe. Leaving it in the frame and
            // reading it from memory at each use is the same mechanism as reading a spilled value,
            // and arrives with the instruction-local legalization that spilling brings.
            incoming = MachineLocation::stack(slot);
        }

        a.incomingArgs.push(incoming);

        // An argument nothing reads is dead the moment it arrives, so it gets no home. Giving it one
        // anyway would reserve a register for it across the entry copies - exactly where a function
        // with more arguments than it uses has the least to spare.
        if(result.uses.isEmpty()) continue;

        a.assign(&result, RegSet {}, incoming);
    }
}

/*
 * The webs an earlier pass displaced, offered a register once nothing else can want one.
 *
 * A displacement is a comparison - this register is worth more to the web asking than to the ones
 * holding it - and the displaced web going homeless is what makes that comparison true. So it may
 * not simply look for another register at its own turn: everything placed after it would then be
 * choosing from a file this web had already taken from, and the shortage moves rather than going
 * away.
 *
 * Once the walk is over there is no such trade left to get wrong. A register still free over a web's
 * whole interval is one every other web in the function was offered and did not take, so handing it
 * over costs nothing anywhere - it is the case §9.1's promotion fixpoint is about, seen from the
 * allocator: `each` in `Pipeline.yana` had three values in the frame with r14 free from entry to
 * exit, because all three had been displaced in the pass that measured the scratch reserve.
 *
 * Hottest first, since two of them can want the one register that is left.
 *
 * Only a web in a *slot*. One that took a recipe is homeless because recreating it is cheaper than
 * a register would be (§5.6), which is a decision about the value and not about the shortage, and it
 * stands whatever is free.
 */
static void reclaimDisplaced(Placer& a) {
    auto& order = a.deferred;
    if(order.isEmpty()) return;

    // Insertion sort: this list is the displaced webs of one function, which kMaxDisplacements bounds
    // at sixteen.
    for(Size i = 1; i < order.size(); i++) {
        auto id = order[i];
        auto cost = a.webs[id].spillCost;
        Size j = i;

        for(; j > 0 && a.webs[order[j - 1]].spillCost < cost; j--) order[j] = order[j - 1];
        order[j] = id;
    }

    for(auto webId: order) {
        auto home = a.homeOfWeb(webId);
        if(!home.isValid() || home.kind != LocationKind::StackSlot) continue;

        auto& info = a.webs[webId];
        auto interval = info.interval();
        if(interval.isEmpty()) continue;

        auto cls = a.out.webs[webId].regClass;
        auto bank = targetRegisters().regClass(cls).bank;
        auto avoid = info.avoid | a.displacedFrom[Size(webId)];

        for(Size i = 0; i < a.orderCount[bank]; i++) {
            auto reg = PhysicalReg { bank, U16(a.order[bank][i]) };
            if(!a.allocatable.has(reg) || avoid.has(reg)) continue;
            if(!targetRegisters().regClass(cls).allowedPhysical.has(reg)) continue;
            if(!a.isFree(webId, cls, reg, interval)) continue;

            // The slot it was holding is left where it is. Nothing else may take it - `takeSlot`
            // recycles by interval overlap and this web's interval has not changed - so handing it
            // back would need the slot list rebuilt for a frame that is at most one slot wider.
            a.occupy(cls, reg, webId);
            a.written.add(reg);
            a.out.webs[webId].segments[0].location = MachineLocation::physical(reg);
            break;
        }
    }
}

/*
 * §5.9 - a register segment around a cluster of uses.
 *
 * §5.8 is this question asked of a web that has a register: what would destroy it is a stretch it
 * has to step out of, and the split buys back the rest. This is the same question asked of a web
 * that has none. Such a web pays a reload at *every* read, however close together its reads are and
 * however hot the block they are in, because the register it would have needed had to be free - and
 * safe - over the whole of its life. Over one stretch of one block, a register that is neither is
 * very often both.
 *
 *      ... call ...            ─┬─  the web avoids every caller-saved register for its whole life
 *      read  [slot]             │
 *      read  [slot]             ├─  and pays a reload at each of these
 *      read  [slot]            ─┴─
 *
 *      ... call ...            ─┬─  the web's *home* is still the slot, at every block boundary
 *      mov  rcx, [slot]         │   one load
 *      read rcx                 ├─  and the reads are register reads
 *      read rcx                 │
 *      read rcx                ─┴─  and nothing is written back: the slot never stopped holding it
 *
 * The direction is the whole of the difference, and three things follow from it.
 *
 * **Nothing is stored back.** The home is a frame slot nobody else may have for the web's whole life
 * (or a recipe, which is always available), so it goes on holding the value while the register does
 * too. Leaving the segment costs nothing at all, which is what `AllocationSegment::cached` says and
 * what makes a two-read cluster already worth taking: one load against two reloads. The rule that
 * makes it true is that no member of the web may be *defined* inside the window - or in the
 * instruction just before it, whose parallel copy is where the load goes.
 *
 * **The register is claimed over the stretch and not over the web's life.** A web reaching here is
 * by construction one no register was free for over its whole interval, so a claim on the whole of
 * it would find nothing every time. `occupyStretch` is what hands out the narrower claim, and it is
 * the "per-segment occupancy" §9 of the README named as this item's prerequisite.
 *
 * **What the register has to survive is what the window covers**, rather than what the web outlives.
 * `info.avoid` is the union over the web's whole life - which is what makes it useless here, since a
 * value live across a call avoids everything a call destroys - so the mask is rebuilt from the
 * clobber sites *inside* the window. The last instruction contributes its `operandMask` rather than
 * its `mask`: the web is read there and dies there, so what has to survive is the copy in front of
 * the instruction and not what the instruction itself goes on to write. At a call those are very
 * different sets, and it is the case this exists for.
 *
 * The window runs from `beforeInst(lo)` to `afterInst(hi)` for the first and last read it covers, so
 * the web is in the register at both of them. The boundary at `beforeInst(lo)` is even and so
 * attaches to instruction lo-1's *post* copies - emitted after lo-1 and before lo, which is where a
 * load has to be - and this is why `lo` may not be a block's first instruction. `hi` may not be a
 * terminator, for §5.8's reason and for one more: the phi transfers at a terminator are sequenced as
 * a batch of their own, and a segment ending inside one would have to be part of it. Between them
 * those two keep the boundary invariant of §1 by construction, exactly as §5.8 does.
 *
 * This runs after `reclaimDisplaced` and last of all, and both halves of that matter. Every web that
 * is getting a register has one by then, so a cluster only ever takes what nothing else asked for -
 * it cannot make another web's decision worse, and there is no ordering here to get wrong. And a
 * whole-life register is strictly better than a cluster of segments, so a web must be offered one
 * first.
 */

// What a load into the register costs, which is the one instruction a window pays for. Materializing
// a recipe costs the same, and is what the window replaces for a web whose home is one.
static U32 clusterEntryCost(MachineLocation home) {
    return home.isStack() ? kReloadCost : kRematCost;
}

// Every read of a candidate web, and every definition of one, in instruction order. Terminators are
// left out: a window may not end at one, and a read at one is a read the window before it has
// already been closed by.
static void collectWebReads(Placer& a, const Array<LiveId>& candidates) {
    auto& reads = a.scratch.reads;
    auto& spans = a.scratch.spans;

    reads.reset(a.webs.size());
    spans.clear();

    // Which candidate each web is, so the walk can skip the ones that are not - most of them.
    auto& wanted = a.scratch.clusterWanted;
    wanted.clear();
    for(Size i = 0; i < a.webs.size(); i++) wanted.push(0);
    for(auto webId: candidates) wanted[Size(webId)] = 1;

    U32 index = 0;

    for(auto offset: a.fun.blocks.contents(a.base)) {
        auto block = a.base[offset];
        auto set = a.live.getBlock(block);
        auto span = U32(spans.size());

        spans.push(BlockSpan {
            .firstIndex = set->firstIndex,
            .lastIndex = set->lastIndex,
            .weight = a.weightOf(block),
        });

        for(auto i: block->instructions.contents(a.base)) {
            auto inst = a.base[i];

            Scratch<InstShape> held(a.shapes);
            auto& shape = *held;
            shapeOf(a.base, a.machine, a.constraints, a.fun, inst, shape);

            // Definitions first, and that is not an arrangement: the search below treats one as a
            // barrier, and an instruction that both defines a member and reads one has to be a
            // barrier for that read too - a result is resolved at its instruction's `before` point,
            // the same point the reads are, so a window ending there would define into the register
            // and leave the home holding the old value.
            for(auto& created: inst->created()) {
                if(isImplicit(&created)) continue;

                auto id = created.liveId();
                if(id == kNullLive || !wanted[Size(a.out.webOf[id])]) continue;

                reads[a.out.webOf[id]].push(WebRead { .index = index, .span = span, .defines = true });
            }

            // The same two questions computeSpillCosts asks, because the saving has to be stated
            // against the price that pass put on this very read.
            auto choice = directMemoryOperands(a.base, a.machine, inst);
            auto inPlace = isInPlace(a, inst, choice);
            auto folded = inPlace ? choice.readWrite : choice.read;

            auto used = inst->used();
            for(Size k = 0; k < used.size(); k++) {
                auto v = a.base[used[k]];
                if(isImplicit(v)) continue;

                auto webId = a.webIdOf(v);
                if(!wanted[Size(webId)]) continue;

                // A read that is copied into a register of somebody else's choosing costs one move
                // wherever the web lives, so a window buys it a shorter encoding and nothing more.
                // Two of them: an operand the encoding pins to a register, and **operand zero of a
                // destructive form**, which is copied into the result's register in front of the
                // instruction whether it comes from a slot, a recipe or another register.
                //
                // The second is the one this had to learn separately - `shape` does not say it, and
                // charging it a whole reload turned `mov [slot],%eax ; mov [slot],%ecx` in `Sort`
                // into a load and two copies, which is one instruction *more* for the same bytes.
                auto tied = k == 0 && a.machine.formOf(inst).tiedResult() == 0
                    && inst->createdCount > 0 && !isImplicit(&inst->created()[0]);

                auto constrained = tied || shape.uses[k].kind == ArgLocation::Register;
                auto home = a.homeOfWeb(webId);

                // What it pays today. A read the encoding pins to a register, or one an addressing
                // form takes straight out of the frame, costs a longer encoding rather than an
                // instruction - so a cluster of those alone never pays for its load.
                auto saving = home.isStack()
                    ? ((constrained || I32(k) == folded) ? kFoldedUseCost : kReloadCost)
                    : (constrained ? kFoldedUseCost : kRematCost);

                reads[webId].push(WebRead { .index = index, .span = span, .saving = saving });
            }

            index++;
        }

        index++; // the terminator, which no window may reach
    }
}

// The registers a window over instructions lo..hi may not use: everything written behind the web's
// back while it has to stay in one, plus - at the last instruction, where it is read and then dies -
// only what the parallel copy in front of that instruction writes.
static RegSet clusterBlockedAt(Placer& a, U32 index, bool last) {
    auto site = a.scratch.siteOfInst[index];
    if(site == kNoSite) return RegSet {};

    auto& clobber = a.clobberSites[site];
    return last ? clobber.operandMask : clobber.mask;
}

static void splitAroundClusters(Placer& a) {
    auto& candidates = a.scratch.clusterWebs;
    candidates.clear();

    for(Size w = 0; w < a.out.webs.size(); w++) {
        auto& web = a.out.webs[w];
        if(web.segments.isEmpty() || web.home().isPhysical()) continue;
        if(a.webs[w].interval().isEmpty()) continue;

        candidates.push(LiveId(w));
    }

    if(candidates.isEmpty()) return;

    // Most to gain first, which is the same order `reclaimDisplaced` takes and for the same reason:
    // two webs can want the one register that is left over a stretch, and the one paying more for
    // it should ask first.
    for(Size i = 1; i < candidates.size(); i++) {
        auto id = candidates[i];
        auto cost = a.webs[id].homelessCost();
        Size j = i;

        for(; j > 0 && a.webs[candidates[j - 1]].homelessCost() < cost; j--) candidates[j] = candidates[j - 1];
        candidates[j] = id;
    }

    collectWebReads(a, candidates);

    auto& convention = a.constraints.getConvention(a.fun.callType);
    auto& windows = a.scratch.clusterWindows;

    for(auto webId: candidates) {
        auto& reads = a.scratch.reads[webId];
        if(reads.size() < 2) continue;

        auto cls = a.out.webs[webId].regClass;
        auto bank = targetRegisters().regClass(cls).bank;
        auto home = a.out.webs[webId].home();
        auto entryCost = clusterEntryCost(home);
        windows.clear();

        Size i = 0;
        while(i + 1 < reads.size()) {
            auto& first = reads[i];
            auto& span = a.scratch.spans[first.span];

            // A definition is a barrier rather than a start, and so is a read at a block's first
            // instruction: the load would have to go in the previous block's terminator.
            if(first.defines || first.index <= span.firstIndex) { i++; continue; }

            // ... as is a definition at this instruction or the one before it. The load joins the
            // previous instruction's parallel copy, which would then both read the slot and be the
            // copy carrying a freshly defined member into it.
            if(i > 0 && reads[i - 1].defines && reads[i - 1].index + 1 >= first.index) { i++; continue; }

            auto lo = first.index;
            auto stretchFrom = beforeInst(lo) - 1;

            RegSet crossed;     // what the web has to survive at every instruction below hi
            U32 masked = lo;    // the first instruction not yet folded into `crossed`
            U32 covered = 0;    // what the reads up to and including `j` pay today

            auto best = kNoRegister;
            Size bestEnd = 0;
            U32 bestGain = 0;

            for(Size j = i; j < reads.size(); j++) {
                if(reads[j].defines || reads[j].span != first.span) break;

                auto hi = reads[j].index;
                for(; masked < hi; masked++) crossed |= clusterBlockedAt(a, masked, false);

                covered += reads[j].saving;
                if(j == i) continue; // one read never pays for its own load

                // What the window would cost against what it saves. The store-and-reload pair §5.8
                // pays is one load here, and the callee-saved registers this function has not
                // already written cost the prologue and epilogue a push and a pop - at the entry's
                // weight, which is why a cold cluster cannot buy one.
                if(covered <= entryCost) continue;
                auto gain = span.weight * (covered - entryCost);

                auto blocked = crossed | clusterBlockedAt(a, hi, true);
                Range stretch { stretchFrom, afterInst(hi) };
                LiveInterval want { &stretch, 1 };

                // First-fit over the same order everything else takes, so the register a window
                // lands on is one no other web wanted over this stretch and one this function
                // already destroys if there is any such left.
                auto found = kNoRegister;

                for(Size k = 0; k < a.orderCount[bank]; k++) {
                    auto reg = PhysicalReg { bank, U16(a.order[bank][k]) };
                    if(!a.allocatable.has(reg) || blocked.has(reg)) continue;
                    if(!targetRegisters().regClass(cls).allowedPhysical.has(reg)) continue;
                    if(!a.isFree(webId, cls, reg, want)) continue;

                    found = a.order[bank][k];
                    break;
                }

                if(found == kNoRegister) continue;

                auto reg = PhysicalReg { bank, U16(found) };
                auto price = convention.calleeSaved.has(reg) && !a.written.has(reg) ? 2 * kStoreCost : 0;
                if(gain <= price || gain - price <= bestGain) continue;

                bestGain = gain - price;
                best = found;
                bestEnd = j;
            }

            if(best == kNoRegister) { i++; continue; }

            auto reg = PhysicalReg { bank, U16(best) };
            auto stretch = Range { stretchFrom, afterInst(reads[bestEnd].index) };

            a.occupyStretch(cls, reg, webId, stretch);
            a.written.add(reg);
            windows.push(ClusterWindow {
                .range = Range { beforeInst(lo), stretch.to },
                .at = MachineLocation::physical(reg),
            });

            // Past every read at the instruction the window closed on, not merely past the one that
            // closed it: another read of the same web at that instruction is already covered, and
            // starting a second window there would produce two segments meeting at one point.
            i = bestEnd + 1;
            while(i < reads.size() && reads[i].index <= reads[bestEnd].index) i++;
        }

        if(windows.isEmpty()) continue;

        // The segment list, home and windows alternating - the same shape setSplit builds, with the
        // home in the frame and the windows in registers rather than the other way round.
        auto interval = a.webs[webId].interval();
        auto& segments = a.out.webs[webId].segments;
        segments.clear();

        auto at = interval.first();

        for(auto& window: windows) {
            // Never below: a read is at or after the web's first live point, and the second window
            // of a web starts past the instruction the first one closed on. Equal is legal and is
            // what a web whose life begins at its first read looks like - the home segment in front
            // of the window is then empty, which `home()` and the segment checks both take.
            assertTrue(window.range.from >= at);
            segments.push(AllocationSegment { .from = at, .to = window.range.from, .location = home });
            segments.push(AllocationSegment {
                .from = window.range.from, .to = window.range.to, .location = window.at, .cached = true,
            });

            at = window.range.to;
        }

        // The last window can end one point past the web's last live point, since it has to cover
        // the `before` of the read that closed it and the interval ends there. The trailing home
        // segment is then empty - and still has to exist, because `locationAt` answers every point
        // past the end with the *last* segment's location and that answer has to be the home. It is
        // what the boundary invariant is checked against.
        segments.push(AllocationSegment {
            .from = at, .to = at > interval.last() ? at : interval.last(), .location = home,
        });
    }
}

// Frame objects that come from the instructions rather than from the signature, plus the two facts
// about the stack that decide whether the function can address its frame through rsp.
static void collectFrameObjects(Placer& a) {
    for(auto offset: a.fun.blocks.contents(a.base)) {
        for(auto i: a.base[offset]->instructions.contents(a.base)) {
            auto inst = a.base[i];

            if(inst->kind == LowerInst::Alloca) {
                auto alloca = (LowerInstAlloca*)inst;
                auto count = a.base[alloca->byteCount];

                if(isImm(count)) {
                    // A compile-time size is an ordinary fixed frame object, and the alloca becomes
                    // an address computation rather than any change to the stack pointer.
                    auto size = ((LowerImm*)count->inst())->i;
                    assertTrue(size > 0 && size <= maxLimit<U32>);

                    auto slot = a.frame.add(StackSlot {
                        .kind = StackSlotKind::Local,
                        .slotClass = StackSlotClass::Slot64,
                        .size = U32(size),

                        // What the program asked for. Frame layout rounds the object up to it and
                        // raises the whole frame's alignment to match, so an over-aligned local is
                        // over-aligned rather than merely large.
                        .alignment = alloca->alignment,
                    });

                    a.frame.references.add(inst, FrameReference { .slot = slot });
                } else {
                    a.frame.hasDynamicAlloca = true;
                }
            }

            if(inst->kind == LowerInst::Call) {
                Scratch<InstShape> held(a.shapes);
            auto& shape = *held;
            shapeOf(a.base, a.machine, a.constraints, a.fun, inst, shape);
                auto& convention = *shape.convention;

                if(convention.stackAlignment > a.frame.callAlignment) {
                    a.frame.callAlignment = convention.stackAlignment;
                }

                // One area serves every call, sized for the hungriest: it is reserved once by the
                // prologue and rewritten by each call's argument stores.
                auto bytes = argAreaBytes(convention, shape.uses);
                if(bytes > a.frame.argAreaSize) a.frame.argAreaSize = bytes;
            }
        }
    }
}

void computePlacement(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
    const Constraints& constraints, const FunctionFrequencyInfo& frequency, bool framePointer,
    const TemporaryReserve& temporaries, const Array<RegSet>& displacedFrom, RegScratch& scratch,
    Placement& out)
{
    if(!scratch.placement) scratch.placement = new PlacementScratch();

    Placer a(base, fun, live, machine, constraints, frequency, framePointer, temporaries, displacedFrom,
        *scratch.placement, out);
    collectFrameObjects(a);

    // Webs before avoid sets: a clobber that one member has to dodge is one the whole web has to
    // dodge, since they share a location. Costs after both, since a web is what carries them and an
    // in-place read-modify-write is a property of two values being one web.
    buildWebs(a);
    computeAvoidSets(a);
    computeSpillCosts(a);

    // Arguments occupy their registers from the entry point, so they are placed before anything
    // else can claim one.
    placeArgs(a, constraints.getConvention(fun.callType));

    U32 index = 0;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        a.currentWeight = a.weightOf(block);

        for(auto i: block->instructions.contents(base)) {
            placeInst(a, base[i], index);
            index++;
        }

        assertTrue(block->terminator != nullptr);
        placeInst(a, base[block->terminator], index);

        // After the terminator's own results, which is the order legalization reads them in: a phi
        // transfer is emitted after whatever the terminator itself needs.
        for(auto successor: block->outgoing) {
            if(!successor) continue;

            assertTrue(base[successor]->phis.isEmpty() || !(block->outgoing[0] && block->outgoing[1]));
            placePhis(a, block, base[successor]);
        }

        index++;
    }

    assertTrue(index == live.instCount); // the walk here and buildRanges' numbering must agree

    // After the walk and nowhere else - see the comment on each of them. The order is the one thing
    // that matters between the two: a whole-life register beats any number of cluster segments, so
    // a web has to have been offered one before it is offered the other.
    reclaimDisplaced(a);
    splitAroundClusters(a);

    // Everything else was written straight into `out` as the walk went; these two are the placer's
    // running totals, and are only the answer once it has finished.
    out.writtenPhysical = a.written;
    out.requiresLegalizationTemps = a.requiresLegalizationTemps;
}
