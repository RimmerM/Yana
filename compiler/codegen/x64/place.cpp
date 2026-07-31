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
static void mergeRanges(Array<Range>& a, const Range* b, Size count) {
    Array<Range> out;
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

    a = ::move(out);
}

static LiveInterval intervalOf(const Array<Range>& ranges) {
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
    // works on a web as on a single value.
    Array<Range> ranges;

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

    LiveInterval interval() const { return intervalOf(ranges); }

    // What losing its register would actually cost this web, which is whichever of the two homeless
    // states it would then choose. This is the number one web is weighed against another by.
    U32 homelessCost() const { return canRemat && rematCost < spillCost ? rematCost : spillCost; }
};

// One clobbering instruction, remembered so that values whose ranges cross it can be kept out of
// the registers it writes. Collected in a first pass because a value has to be placed before the
// walk reaches the instructions it outlives.
struct ClobberSite {
    U32 index;
    RegSet mask;

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

struct Placer {
    LowerBase base;
    LowerFunction& fun;
    Liveness& live;
    const MachineFunction& machine;
    const Constraints& constraints;

    // Where the four walks below get the instruction shape they each ask for per instruction. A
    // pool rather than one shape, because two of them nest - see Scratch.
    ScratchPool<InstShape> shapes;

    // How often each block runs relative to the function's entry - see FunctionFrequencyInfo. Every
    // decision here that trades one part of the function against another is weighed by it: what a
    // web costs if it is left homeless, and which of two competing phi copies is worth coalescing.
    // Computed once per allocation rather than per placement pass, since nothing on the displacement
    // loop changes the CFG or the edge metadata it is derived from.
    const FunctionFrequencyInfo& frequency;

    // The result, built as the walk goes rather than copied out at the end - which is what lets the
    // operand rule below ask where a sibling operand will be read from while placement is still
    // running, against exactly the structure legalization will read afterwards.
    Placement out;

    // What placement needs to know about each web and nothing downstream does: its merged interval,
    // what it has to stay out of, and what it would cost to leave homeless. Indexed alike with
    // `out.webs`, so a web id names both. Which web each value belongs to is `out.webOf`, which is
    // union-find while the webs are being built and a direct index once they are.
    Array<WebInfo> webs;

    // Everything already placed in each register. A list rather than a single occupant because
    // intervals have holes: several webs can share one register over the function as long as no two
    // of them are ever live at the same point.
    Array<LiveId> occupants[kRegisterBankCount][kMaxRegistersPerBank];

    // The pairs of webs that may not share a register for a reason interval overlap does not state -
    // see collectTieConflicts. Indexed by web id, holding web ids, and symmetric: whichever of the
    // two is placed first, the second finds the register taken.
    //
    // Built by buildWebs, which needs the same relation to decide what it may merge. That it answers
    // both questions is the point: a destructive result and a sibling operand of its instruction may
    // no more share a register than share a web, and a rule that only refused the merge left the
    // register sharing to be discovered by whichever of the two happened to be placed second.
    Array<Array<LiveId>> tieConflicts;

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
    // to frame layout, which is what turns any of it into an address.
    FrameObjects frame;

    // What each spill slot has already been promised to, so that a slot can be reused whenever the
    // stretches do not overlap - the same rule that lets two webs share a register, and what keeps
    // the frame as small as the peak of simultaneously spilled values rather than as large as their
    // total. Ranges rather than webs, because a split web occupies its slot over its windows alone
    // and a homeless one over the whole of its life, and the slot cannot tell the two apart.
    Array<Array<Range>> slotOccupants;

    // Every instruction that writes registers behind its operands' backs, in index order - see
    // ClobberSite. Kept because a split is a decision about which of them a web has to dodge and
    // which it can step around.
    Array<ClobberSite> clobberSites;

    // The recipes for the webs that live nowhere at all - see Remat in gen.h. A web's home names
    // its position here.
    Array<Remat> remats;

    // Where each argument arrived, in argument order - see Placement::incomingArgs.
    Array<MachineLocation> incomingArgs;

    // Webs this pass has to leave homeless whatever it would otherwise have done, because a previous
    // pass found something that wanted their register more. *Homeless* rather than spilled: which of
    // the two homeless states such a web takes is still its own choice, and a cheap constant takes a
    // recipe rather than a slot. Indexed by web id; see the displacement comment on `assign` and the
    // loop in allocateRegisters.
    const Array<bool>& forcedHomeless;

    // Webs *this* pass would rather have displaced than the one it displaced instead. Placement is
    // one walk in the order legalization will later read it, so a web already placed has already
    // been offered to everything that could have taken its register from it - the request is carried
    // out to allocateRegisters and applied to the next pass.
    Array<LiveId> displacementRequests;

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
        const TemporaryReserve& temporaries, const Array<bool>& forcedHomeless):
        base(base), fun(fun), live(live), machine(machine), constraints(constraints),
        frequency(frequency), forcedHomeless(forcedHomeless), temporaries(temporaries)
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

        for(Size i = 0; i < live.valueMap.size(); i++) {
            out.webOf.push(LiveId(i));
            out.webs.push(WebAllocation {});
            tieConflicts.push();

            WebInfo web;
            auto interval = live.getInterval(LiveId(i));
            for(U32 r = 0; r < interval.count; r++) web.ranges.push(interval.ranges[r]);

            webs.push(::move(web));
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
    MachineLocation setSplit(LiveId webId, RegisterClassId cls, MachineLocation home,
        const Array<Range>& windows, MachineLocation windowLocation)
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
    bool isFree(LiveId webId, RegisterClassId cls, PhysicalReg reg, const LiveInterval& interval) {
        auto units = targetRegisters().viewOf(cls, reg).units;

        for(Size i = 0; units; i++, units >>= 1) {
            if(!(units & 1)) continue;

            for(auto id: occupants[reg.bank][i]) {
                if(webs[id].interval().overlaps(interval)) return false;
                if(tiesConflict(webId, id)) return false;
            }
        }

        return true;
    }

    // A register a web has been given is held for the whole of that web's interval, windows
    // included. A split web is not *in* its register over a window, but the copies at either end of
    // one read and write it there, so it is not free to hold anything else either - and what could
    // fit is nothing: a window is one instruction wide, and the only value whose whole life fits
    // inside one is a result nothing reads. Handing a window's worth of register back is what a
    // window in a *preserved register* rather than the frame would need, and what would make this
    // per-segment.
    void occupy(RegisterClassId cls, PhysicalReg reg, LiveId webId) {
        auto units = targetRegisters().viewOf(cls, reg).units;

        for(Size i = 0; units; i++, units >>= 1) {
            if(units & 1) occupants[reg.bank][i].push(webId);
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
        auto avoid = info.avoid | extraAvoid;
        auto interval = info.interval();

        // A previous pass found something that needed this web's register more than it did.
        if(forcedHomeless[webId]) return assignHomeless(webId, v->type, cls, interval);

        auto usable = [&](Size i, const RegSet& blocked) {
            auto reg = PhysicalReg { bank, U16(i) };
            if(!allocatable.has(reg) || blocked.has(reg)) return false;
            if(!targetRegisters().regClass(cls).allowedPhysical.has(reg)) return false;
            return isFree(webId, cls, reg, interval);
        };

        auto chosen = kNoRegister;

        if(hint.isPhysical() && hint.bank == bank && usable(hint.index, avoid)) {
            chosen = hint.index;
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
        Array<Range> windows; // sorted, disjoint, and strictly inside the web's interval
        U32 cost = 0;         // what the windows cost, in computeSpillCosts' units
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
        Array<Size> crossed;
        auto blocked = info.avoidFixed | extraAvoid;

        for(Size i = 0; i < clobberSites.size(); i++) {
            if(!interval.crosses(clobberSites[i].index)) continue;

            if(coverable(clobberSites[i], webId)) crossed.push(i);
            else blocked |= clobberSites[i].mask;
        }

        if(crossed.isEmpty()) return false;

        auto best = kNoRegister;
        U32 bestCost = 0;
        Array<Range> bestWindows;

        for(Size k = 0; k < orderCount[bank]; k++) {
            auto i = order[bank][k];
            if(!usable(i, blocked)) continue;

            Array<Range> windows;
            U32 cost = 0;
            windowsFor(PhysicalReg { bank, U16(i) }, crossed, windows, cost);

            // A register needing no window is one the unsplit search would already have taken.
            assertTrue(!windows.isEmpty());

            if(best == kNoRegister || cost < bestCost) {
                best = i;
                bestCost = cost;
                bestWindows = ::move(windows);
            }
        }

        if(best == kNoRegister) return false;

        out_.reg = best;
        out_.cost = bestCost;
        out_.windows = ::move(bestWindows);
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

        auto slot = takeSlot(stackSlotClassFor(type), intervalOf(plan.windows));
        return setSplit(webId, cls, MachineLocation::physical(reg), plan.windows, MachineLocation::stack(slot));
    }

    // Whether this occupant would have to go for `webId` to take the register it is in. Every
    // occupant whose life overlaps, and - for the same reason isFree refuses one - every occupant
    // the web ties against, whose life does not overlap and which would still be read after the
    // copy in front of its instruction had overwritten it.
    bool displaces(LiveId webId, LiveId occupant, const LiveInterval& interval) const {
        return webs[occupant].interval().overlaps(interval) || tiesConflict(webId, occupant);
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
            for(auto id: occupants[bank][i]) {
                if(displaces(webId, id, interval)) cost += webs[id].homelessCost();
            }

            if(cost < bestCost) { bestCost = cost; best = i; }
        }

        return best;
    }

    void recordDisplacement(LiveId webId, RegisterBankId bank, Size reg, const LiveInterval& interval) {
        for(auto id: occupants[bank][reg]) {
            if(displaces(webId, id, interval)) displacementRequests.push(id);
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

        return setHome(webId, cls, MachineLocation::stack(takeSlot(stackSlotClassFor(type), interval)));
    }

    // A slot in the frame for the stretches `ranges` covers, reusing one nothing is using over any of
    // them. Slots are recycled by exactly the rule registers are, so the frame ends up as large as
    // the peak number of simultaneously spilled webs rather than as large as their total - and a
    // split web's window, being a stretch and not a life, shares slots with whatever is dead there.
    StackSlotId takeSlot(StackSlotClass slotClass, const LiveInterval& ranges) {
        while(slotOccupants.size() < frame.slots.size()) slotOccupants.push();

        auto claim = [&](StackSlotId slot) {
            mergeRanges(slotOccupants[slot], ranges.ranges, ranges.count);
            return slot;
        };

        for(Size i = 0; i < frame.slots.size(); i++) {
            if(frame.slots[i].kind != StackSlotKind::Spill) continue;
            if(frame.slots[i].slotClass != slotClass) continue;
            if(intervalOf(slotOccupants[i]).overlaps(ranges)) continue;

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
using TieConflicts = Array<Array<LiveId>>;

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
    for(Size i = 0; i < a.out.webOf.size(); i++) out.push();

    auto onInst = [&](LowerInst* inst) {
        if(a.machine.formOf(inst).tiedResult() != 0) return;
        if(inst->createdCount == 0 || isImplicit(&inst->created()[0])) return;

        auto used = inst->used();
        if(used.size() == 0) return;

        auto result = inst->created()[0].liveId();

        for(Size i = 1; i < used.size(); i++) {
            auto value = a.base[used[i]];
            if(isImplicit(value) || used[i] == used[0]) continue;

            out[result].push(value->liveId());
            out[value->liveId()].push(result);
        }
    };

    for(auto offset: a.fun.blocks.contents(a.base)) {
        auto block = a.base[offset];
        for(auto i: block->instructions.contents(a.base)) onInst(a.base[i]);
        onInst(a.base[block->terminator]);
    }
}

static void buildWebs(Placer& a) {
    // Union-find over values, with the web's merged interval kept on the representative so that the
    // interference test is against everything already merged into it rather than against one member.
    Array<LiveId> parent;
    for(Size i = 0; i < a.out.webOf.size(); i++) parent.push(LiveId(i));

    auto find = [&](LiveId id) {
        while(parent[id] != id) {
            parent[id] = parent[parent[id]]; // halve the path as we go
            id = parent[id];
        }

        return id;
    };

    TieConflicts tieConflicts;
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

    Array<Size> blockOrder;
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

                // The one thing that has to hold: a web is a single location, so no two values in
                // it may ever be live at once.
                if(a.webs[left].interval().overlaps(a.webs[right].interval())) continue;

                // And the one thing that does not follow from it - see collectTieConflicts.
                if(tiesConflict(left, right)) continue;

                mergeRanges(a.webs[left].ranges, a.webs[right].ranges.pointer(), a.webs[right].ranges.size());
                a.webs[right].ranges.clear();

                for(auto id: tieConflicts[right]) tieConflicts[left].push(id);
                tieConflicts[right].clear();

                parent[right] = left;
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
 * Pass 1: work out which registers each value has to stay out of.
 */

static void computeAvoidSets(Placer& a) {
    U32 index = 0;

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
                // An operand that the parallel copy in front of this instruction does *not* place
                // is read straight out of its own register, so that register has to survive both
                // the copy and whatever the instruction's expansion writes before reading its
                // sources (`xor rdx, rdx` ahead of a division, r11 as scratch in an unrolled copy).
                //
                // This half is `avoidFixed` as well: it is the operand's own instruction, so there
                // is no window that could carry the web past it - it has to be readable *here*.
                auto used = inst->used();
                for(Size i = 0; i < used.size(); i++) {
                    auto v = a.base[used[i]];
                    if(isImplicit(v)) continue;
                    if(shape.uses[i].kind != ArgLocation::None) continue;

                    auto& web = a.webFor(v);
                    web.avoid |= mask;
                    web.avoidFixed |= mask;
                }

                // A return ends the function, so nothing can be live across it.
                if(!shape.isReturn) {
                    a.clobberSites.push(ClobberSite {
                        .index = index,
                        .mask = mask,
                        .inst = inst,
                        .weight = weight,
                        .terminator = terminator,
                    });
                }
            }

            index++;
        };

        for(auto i: block->instructions.contents(a.base)) onInst(a.base[i], false);
        onInst(a.base[block->terminator], true);
    }

    // The other half: a clobber a web merely outlives. This is the part a split can buy back, and
    // the only part - which is why it lands in `avoid` alone and not in `avoidFixed`.
    for(auto& web: a.webs) {
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
            // A float constant would have to be loaded from a pool, which nothing builds yet.
            if(!isIntLike(v->type)) return false;

            out = Remat { .kind = Remat::Immediate, .type = v->type, .imm = ((LowerImm*)inst)->i };
            return true;

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
    Array<U16> members;
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

    Array<ArgLocation> locations;
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

Placement computePlacement(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
    const Constraints& constraints, const FunctionFrequencyInfo& frequency, bool framePointer,
    const TemporaryReserve& temporaries, const Array<bool>& forcedHomeless)
{
    Placer a(base, fun, live, machine, constraints, frequency, framePointer, temporaries, forcedHomeless);
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

    auto result = ::move(a.out);
    result.frame = ::move(a.frame);
    result.remats = ::move(a.remats);
    result.incomingArgs = ::move(a.incomingArgs);
    result.writtenPhysical = a.written;
    result.requiresLegalizationTemps = a.requiresLegalizationTemps;
    result.displacementRequests = ::move(a.displacementRequests);
    return result;
}
