#include "gen.h"
#include "x64_util.h"

/*
 * Register allocation.
 *
 * Every web is given one location - a register, a slot in the frame, or a recipe that recreates the
 * value wherever it is needed - for the whole of its life, and keeps it. Nothing is ever relocated
 * mid-function, so a value is in the same place on every path that reaches a given instruction,
 * which is what makes the result independent of how the blocks happen to be laid out.
 *
 * The inputs both come from lower_analyze.cpp: a linear numbering of the instructions (in the order
 * LowerFunction::blocks lists the blocks, which transformFunction has put in reverse postorder), and
 * a live interval per value in that numbering. Two webs may share a location exactly when their
 * intervals never overlap.
 *
 * A web that cannot be given a register does not get moved around to make one available. It is given
 * one of the two homeless states instead - a slot in the frame, or a recipe that recreates it
 * wherever it is read - and brought into a scratch register at the instructions that cannot work
 * with it where it is.
 *
 * The pipeline is four passes:
 *
 *   0. buildWebs         - phi-related values that provably never overlap become one web, so the
 *                          copy between them is an identity and disappears.
 *   1. computeAvoidSets  - which registers each web has to stay out of, because something writes
 *                          them while the web is live.
 *   1b. computeSpillCosts - what each web would cost in either homeless state, so that a register
 *                          can be taken from whichever web values it least.
 *   2. Emitter           - place each web and record, per instruction, where the encoder finds
 *                          every operand.
 *
 * That leaves five things a location cannot simply be handed out for, all handled by *copying*
 * around the awkward place rather than by moving a web's home:
 *
 *   - Fixed-register constraints (a divisor in rax, a call argument in rdi, ...). Operands are
 *     copied into place before the instruction and results copied out of place after it, so the
 *     web's home is unaffected. The copies are emitted as one parallel copy per instruction.
 *   - Clobbers. A web whose interval *crosses* a clobbering instruction is simply never given one
 *     of the clobbered registers (see ClobberSite below), so there is nothing to rescue at the call.
 *   - Destructive two-address encodings, where the result overwrites its first operand's register.
 *     The result is allocated first, preferring that operand's register, and the operand is copied
 *     into the result's register when they differ.
 *   - A home in the frame. Most encoders cannot read one, so the value is loaded into a scratch
 *     register before the instruction and stored back after it if the instruction wrote it. Where
 *     the encoding does have a memory form, it is used and neither exists.
 *   - A home that is a recipe. Nothing holds the value at all, so it is recreated into a scratch
 *     register wherever it is read, and the instruction that would have defined it emits nothing.
 *
 * The scratch registers are reserved by a second allocation attempt - see kMaxSpillTemps.
 *
 * Phis are ordinary values here. A phi that shares a web with the value arriving over an edge needs
 * nothing at all; otherwise its location is decided at the first predecessor edge that reaches it,
 * and each predecessor ends with a parallel copy placing the incoming values into the phi locations.
 * transformFunction guarantees a block that needs such a copy has exactly one successor, so the copy
 * cannot run on a path that skips the phis.
 *
 * The result is checked before it is returned: verify.cpp simulates what the emitted code will leave
 * in each register and slot and confirms every instruction reads a location that actually holds the
 * value it wants. That runs in debug builds only, and it is the thing to reach for first when any of
 * this changes - it turns "wrong code in a shape nothing tests" into an assertion.
 */

// Instruction kinds whose x86 encoding is destructive two-address: the result is written over the
// register holding used()[0]. Mul/Div/Rem are excluded - their result register is forced by
// InstConstraints instead, regardless of where the first operand sits.
//
// IMul is the one kind where this depends on the operands: `imul r, r/m, imm` is a true
// three-operand form, so only the register-by-register encoding is destructive. genIMul picks the
// same way round.
static bool isDestructive(LowerBase base, LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Neg: case LowerInst::Not: case LowerInst::X86Bswap:
        case LowerInst::Add: case LowerInst::Sub:
        case LowerInst::Shl: case LowerInst::Shr: case LowerInst::Sar:
        case LowerInst::And: case LowerInst::Or: case LowerInst::Xor:
        case LowerInst::Select:
            return true;
        case LowerInst::IMul:
            return !isImm(base[((LowerInstBinary*)inst)->rhs]);
        default:
            return false;
    }
}

/*
 * Parallel copies.
 */

// Whether some other transfer still to be emitted overwrites the source of transfer `i` - which is
// what makes `i` part of a cycle rather than something merely blocked by one.
static bool writesSource(const Array<RegMove>& pending, const Array<bool>& done, Size i) {
    for(Size j = 0; j < pending.size(); j++) {
        if(j == i || done[j]) continue;
        if(pending[j].to == pending[i].from) return true;
    }

    return false;
}

// Sequences a set of simultaneous copies into an order that executes them one at a time without any
// of them destroying a value another still has to read. A copy can be emitted as soon as nothing
// left in the set reads its destination; when nothing qualifies, what remains is a permutation
// cycle, and it has to be broken.
//
// Two ways to break one. Between registers, `xchg` does it in a single instruction and needs nothing
// to go through, which is what makes cycle-breaking unable to fail for lack of a register. With a
// frame slot at either end there is no exchange to use, so the destination is saved into a scratch
// register first and whoever was waiting to read it reads the scratch instead.
//
// A transfer with a slot at both ends - two spilled webs feeding the same phi - is expanded
// afterwards, since x86 has no memory-to-memory move. So is one out of a recipe and into a slot,
// for the same reason: the value has to exist in a register before anything can store it.
static void sequenceMoves(Array<RegMove>& pending, Array<RegMove>& out) {
    Array<bool> done;
    for(Size i = 0; i < pending.size(); i++) done.push(pending[i].from == pending[i].to);

    auto begin = out.size();

    for(;;) {
        bool progress = false;
        bool remaining = false;

        for(Size i = 0; i < pending.size(); i++) {
            if(done[i]) continue;

            bool blocked = false;
            for(Size j = 0; j < pending.size(); j++) {
                if(j == i || done[j]) continue;
                if(pending[j].from == pending[i].to) { blocked = true; break; }
            }

            if(blocked) {
                remaining = true;
                continue;
            }

            out.push(pending[i]);
            done[i] = true;
            progress = true;
        }

        if(!remaining) break;

        if(!progress) {
            // Break a transfer that is genuinely part of the cycle - one whose own source something
            // else is going to overwrite. A transfer merely *pointing into* the cycle would be
            // broken to no purpose, and would consume the scratch register the cycle itself needs.
            // A recipe is never anyone's destination, so a move out of one is never picked here.
            Size i = 0;
            while(done[i] || !writesSource(pending, done, i)) {
                i++;
                if(i == pending.size()) { i = 0; while(done[i]) i++; break; }
            }

            auto& move = pending[i];
            done[i] = true;

            RegId reads;

            if(isSlot(move.from) || isSlot(move.to) || isRemat(move.from)) {
                // No exchange to reach for: park the destination somewhere first.
                auto scratch = moveTemp(GenReg, 0);
                out.push(RegMove { move.to, scratch });
                out.push(RegMove { move.from, move.to });
                reads = scratch;
            } else {
                out.push(RegMove { move.from, move.to, true });
                reads = move.from;
            }

            for(Size j = 0; j < pending.size(); j++) {
                if(!done[j] && pending[j].from == move.to) pending[j].from = reads;
            }
        }
    }

    // Expand any remaining transfer that has to go through a register into a load (or a
    // materialization) and a store. Done here rather than during sequencing so that the ordering
    // above is decided on the transfers the caller asked for, and each expansion stays an adjacent
    // pair - which is what lets them all share one scratch.
    for(auto i = begin; i < out.size(); i++) {
        if(isPhysicalLocation(out[i].from) || !isSlot(out[i].to)) continue;

        auto scratch = moveTemp(GenReg, 1);
        auto to = out[i].to;

        out[i].to = scratch;
        out.insert(i + 1, RegMove { scratch, to });
        i++;
    }
}

/*
 * Allocation state.
 */

// One clobbering instruction, remembered so that values whose ranges cross it can be kept out of
// the registers it writes. Collected in a first pass because a value has to be placed before the
// walk reaches the instructions it outlives.
struct ClobberSite {
    U32 index;
    RegSet mask;
};

/*
 * Webs.
 *
 * Allocation is over webs rather than over values. A web is a set of values that a phi ties together
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

    RegId home = kInvalidReg;

    // What this web costs if it does not get a register, by either of the two ways of not having
    // one - see computeSpillCosts. Both are in the same units, so they can be compared with each
    // other and with another web's.
    U32 spillCost = 0;
    U32 rematCost = 0;

    // Set when the web is one definition of a value cheap enough to recreate wherever it is read,
    // and `recipe` is how - see Remat in gen.h.
    bool canRemat = false;
    Remat recipe;

    LiveInterval interval() const { return LiveInterval { ranges.pointer(), U32(ranges.size()) }; }

    // What losing its register would actually cost this web, which is whichever of the two homeless
    // states it would then choose. This is the number one web is weighed against another by.
    U32 homelessCost() const { return canRemat && rematCost < spillCost ? rematCost : spillCost; }
};

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
 */
static void buildOrder(const CallConvention& convention, RegClass cls, U16* out) {
    Size count = 0;
    auto framePointer = framePointerReg();

    auto append = [&](Size i) {
        if(makeRegId(cls, U16(i)) == framePointer) return; // last, below
        out[count++] = U16(i);
    };

    for(Size i = 0; i < kRegCount; i++) {
        if(convention.clobber.has(makeRegId(cls, U16(i)))) append(i);
    }

    for(Size i = 0; i < kRegCount; i++) {
        if(!convention.clobber.has(makeRegId(cls, U16(i)))) append(i);
    }

    if(cls == getRegClass(framePointer)) out[count++] = getRegIndex(framePointer);
    assertTrue(count == kRegCount); // every register is in the order exactly once
}

struct Allocator {
    LowerBase base;
    LowerFunction& fun;
    Liveness& live;
    const Constraints& constraints;

    // Which web each value belongs to, indexed by LiveId, and the webs themselves. Union-find while
    // webs are being built; flattened to a direct index once they are.
    Array<LiveId> webOf;
    Array<WebInfo> webs;

    // Everything already placed in each register. A list rather than a single occupant because
    // intervals have holes: several webs can share one register over the function as long as no two
    // of them are ever live at the same point.
    Array<LiveId> occupants[kPhysRegClassCount][kRegCount];

    // The registers a value can be handed, held once rather than rebuilt at every assignment.
    RegSet allocatable = allocatableRegs();

    // The order to try them in, per class - see buildOrder. Registers this function's own
    // convention lets it destroy come first, since taking one of those costs nothing at all.
    U16 order[kPhysRegClassCount][kRegCount] = {};

    // Every register the function writes: the ones handed out to values, plus the ones instructions
    // clobber or are forced to write behind a value's back. The callee-saved ones among them are
    // what the prologue has to save (see FunctionRegs::usedCalleeSaved) - a register that is
    // clobbered is just as destroyed from the caller's point of view as one holding a value, so
    // both sources count.
    RegSet written;

    // Everything the function needs stack space for. Filled in as the reasons appear - an argument
    // the caller left on the stack, an alloca, a web that could not be given a register - and handed
    // to frame layout, which is what turns any of it into an address.
    FrameObjects frame;

    // The webs living in each spill slot, so that a slot can be reused by webs whose lives do not
    // overlap - the same rule that lets two webs share a register, and what keeps the frame as small
    // as the peak of simultaneously spilled values rather than as large as their total.
    Array<Array<LiveId>> slotOccupants;

    // The recipes for the webs that live nowhere at all - see Remat in gen.h. A web's home names
    // its position here.
    Array<Remat> remats;

    // Webs this attempt has to leave homeless whatever it would otherwise have done, because a
    // previous attempt found something that wanted their register more. Indexed by web id; see the
    // eviction comment on `assign` and the loop in allocateRegisters.
    const Array<bool>& forceSpill;

    // Webs *this* attempt would rather have displaced than the one it displaced instead. It cannot
    // act on that itself - a web already placed has already been emitted into the instructions that
    // read it - so the answer is carried out to allocateRegisters and applied to the next attempt.
    Array<LiveId> evicted;

    // Set when any web ended up without a register. The first attempt at a function reserves no
    // scratch registers; if this comes back true, the whole function is allocated again with some
    // held back, because a value that is not in a register has to be brought into one at each
    // instruction that touches it. See allocateRegisters.
    bool needsScratch = false;

    Allocator(LowerBase base, LowerFunction& fun, Liveness& live, const Constraints& constraints, RegSet reserved, const Array<bool>& forceSpill):
        base(base), fun(fun), live(live), constraints(constraints), forceSpill(forceSpill)
    {
        // Whatever is held back - the scratch registers, and rbp in a function that establishes a
        // frame pointer - is not available to hand out as a home.
        allocatable = reserved.complement(allocatable);

        auto& convention = constraints.getConvention(fun.callType);
        for(Size cls = 0; cls < kPhysRegClassCount; cls++) buildOrder(convention, RegClass(cls), order[cls]);

        for(Size i = 0; i < live.valueMap.size(); i++) {
            webOf.push(LiveId(i));

            WebInfo web;
            auto interval = live.getInterval(LiveId(i));
            for(U32 r = 0; r < interval.count; r++) web.ranges.push(interval.ranges[r]);

            webs.push(::move(web));
        }
    }

    LiveId webIdOf(LowerValue* v) {
        auto id = v->liveId();
        assertTrue(id != kNullLive); // every non-implicit value is numbered by buildLiveness
        return webOf[id];
    }

    WebInfo& webFor(LowerValue* v) { return webs[webIdOf(v)]; }

    // Whether two values ended up sharing a location, which is what makes the copy between them an
    // identity that never has to be emitted.
    bool sameWeb(LowerValue* a, LowerValue* b) { return webIdOf(a) == webIdOf(b); }

    // Where a value lives. Reading this before its web has been placed means the walk reached a use
    // before the definition, which the reverse-postorder block order rules out.
    RegId homeOf(LowerValue* v) {
        auto home = webFor(v).home;
        assertTrue(home != kInvalidReg);
        return home;
    }

    // A register is available to a web when nothing already in it is ever live at the same time.
    // Comparing whole intervals rather than an endpoint against the walk's current position is what
    // lets a phi be placed at whichever predecessor edge reaches it first, and what lets a register
    // be reused across a web's holes.
    bool isFree(RegClass cls, Size index, const LiveInterval& interval) {
        for(auto id: occupants[cls][index]) {
            if(webs[id].interval().overlaps(interval)) return false;
        }

        return true;
    }

    // Places the web `v` belongs to, preferring `hint` so that a copy the encoder would otherwise
    // have to emit collapses into a no-op. A web that is already placed keeps what it has: every
    // value in it is one definition of a single quantity living in a single location.
    RegId assign(LowerValue* v, RegSet extraAvoid, RegId hint) {
        auto webId = webIdOf(v);
        auto& info = webs[webId];

        if(info.home != kInvalidReg) return info.home;

        auto cls = classForType(v->type);
        auto avoid = info.avoid | extraAvoid;
        auto interval = info.interval();

        // A previous attempt found something that needed this web's register more than it did.
        if(forceSpill[webId]) return assignHomeless(webId, v->type, interval);

        auto usable = [&](Size i) {
            auto reg = makeRegId(cls, U16(i));
            if(!allocatable.has(reg) || avoid.has(reg)) return false;
            return isFree(cls, i, interval);
        };

        Size chosen = kRegCount;

        if(hint != kInvalidReg && getRegClass(hint) == cls && usable(getRegIndex(hint))) {
            chosen = getRegIndex(hint);
        } else {
            for(auto i: order[cls]) {
                if(usable(i)) { chosen = i; break; }
            }
        }

        // No register is free for the whole of this web's life. Before settling for that, ask what
        // the webs standing in the way are worth: a value read once outside a loop is a far better
        // thing to displace than one read at every iteration inside one.
        //
        // The displaced web cannot be moved where it stands - it was placed while this same walk was
        // emitting the instructions that read it, and its location is already written into them - so
        // it is *recorded* instead, and the next attempt starts with it homeless, which leaves its
        // register free at the point that wanted it. A function that gets this far is being
        // allocated more than once regardless.
        if(chosen == kRegCount) {
            recordEviction(cls, avoid, interval, info.homelessCost());
            return assignHomeless(webId, v->type, interval);
        }

        info.home = makeRegId(cls, chosen);
        occupants[cls][chosen].push(webId);
        written.add(info.home);
        return info.home;
    }

    // Notes the register whose occupants would be cheapest to displace, if displacing them costs
    // less than `budget` - what the web asking for a register is about to pay for not having one.
    // Every occupant whose life overlaps has to go, since the register is only free for the new web
    // when all of them have.
    void recordEviction(RegClass cls, const RegSet& avoid, const LiveInterval& interval, U32 budget) {
        Size best = kRegCount;
        auto bestCost = budget;

        for(auto i: order[cls]) {
            auto reg = makeRegId(cls, U16(i));
            if(!allocatable.has(reg) || avoid.has(reg)) continue;

            U32 cost = 0;
            for(auto id: occupants[cls][i]) {
                if(webs[id].interval().overlaps(interval)) cost += webs[id].homelessCost();
            }

            if(cost < bestCost) { bestCost = cost; best = i; }
        }

        if(best == kRegCount) return;

        for(auto id: occupants[cls][best]) {
            if(webs[id].interval().overlaps(interval)) evicted.push(id);
        }
    }

    // A web that is not getting a register. There are two ways to do without one and they cost
    // differently: recreating the value wherever it is read, or keeping it in the frame and bringing
    // it back at each instruction that cannot read a memory operand. computeSpillCosts priced both.
    RegId assignHomeless(LiveId webId, LowerType type, const LiveInterval& interval) {
        auto& info = webs[webId];
        needsScratch = true;

        if(info.canRemat && info.rematCost < info.spillCost) {
            remats.push(info.recipe);
            info.home = makeRegId(RematReg, U16(remats.size() - 1));
            return info.home;
        }

        return assignSlot(webId, type, interval);
    }

    // Gives a web a slot in the frame, reusing one whose current occupants are never live at the
    // same time. Slots are recycled by exactly the rule registers are, so the frame ends up as large
    // as the peak number of simultaneously spilled webs rather than as large as their total.
    RegId assignSlot(LiveId webId, LowerType type, const LiveInterval& interval) {
        auto& info = webs[webId];
        auto slotClass = stackSlotClassFor(type);

        while(slotOccupants.size() < frame.slots.size()) slotOccupants.push();

        for(Size i = 0; i < frame.slots.size(); i++) {
            if(frame.slots[i].kind != StackSlotKind::Spill) continue;
            if(frame.slots[i].slotClass != slotClass) continue;

            bool free = true;
            for(auto id: slotOccupants[i]) {
                if(webs[id].interval().overlaps(interval)) { free = false; break; }
            }

            if(!free) continue;

            slotOccupants[i].push(webId);
            info.home = makeRegId(StackReg, U16(i));
            return info.home;
        }

        auto size = stackSlotSize(slotClass);
        auto slot = frame.add(StackSlot {
            .kind = StackSlotKind::Spill,
            .slotClass = slotClass,
            .size = size,
            .alignment = size,
        });

        while(slotOccupants.size() <= slot) slotOccupants.push();
        slotOccupants[slot].push(webId);

        info.home = makeRegId(StackReg, slot);
        return info.home;
    }
};

/*
 * Pass 0: build the webs.
 */

// Merges `b`'s ranges into `a`'s, keeping the result sorted and disjoint. Both are already sorted,
// so this is one merge walk with adjacent ranges folded together.
static void mergeRanges(Array<Range>& a, const Array<Range>& b) {
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

    while(i < a.size() && j < b.size()) {
        if(a[i].from <= b[j].from) append(a[i++]);
        else append(b[j++]);
    }

    while(i < a.size()) append(a[i++]);
    while(j < b.size()) append(b[j++]);

    a = ::move(out);
}

static void buildWebs(Allocator& a) {
    // Union-find over values, with the web's merged interval kept on the representative so that the
    // interference test is against everything already merged into it rather than against one member.
    Array<LiveId> parent;
    for(Size i = 0; i < a.webOf.size(); i++) parent.push(LiveId(i));

    auto find = [&](LiveId id) {
        while(parent[id] != id) {
            parent[id] = parent[parent[id]]; // halve the path as we go
            id = parent[id];
        }

        return id;
    };

    // Block order, so that the result is reproducible in the goldens. Processing the hottest phis
    // first would coalesce the copies that matter most when two candidates compete, and is the
    // natural refinement once block frequencies exist.
    for(auto offset: a.fun.blocks.contents(a.base)) {
        auto block = a.base[offset];

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

                mergeRanges(a.webs[left].ranges, a.webs[right].ranges);
                a.webs[right].ranges.clear();
                parent[right] = left;
            }
        }
    }

    // Flatten, so that everything afterwards is a direct lookup rather than a find().
    for(Size i = 0; i < a.webOf.size(); i++) a.webOf[i] = find(LiveId(i));
}

/*
 * Pass 1: work out which registers each value has to stay out of.
 */

static void computeAvoidSets(Allocator& a) {
    Array<ClobberSite> sites;
    U32 index = 0;

    for(auto offset: a.fun.blocks.contents(a.base)) {
        auto block = a.base[offset];

        auto onInst = [&](LowerInst* inst) {
            auto shape = shapeOf(a.base, a.constraints, a.fun, inst);
            auto mask = writtenRegisters(shape);
            a.written |= mask;

            if(!mask.isEmpty()) {
                // An operand that the parallel copy in front of this instruction does *not* place
                // is read straight out of its own register, so that register has to survive both
                // the copy and whatever the instruction's expansion writes before reading its
                // sources (`xor rdx, rdx` ahead of a division, r11 as scratch in an unrolled copy).
                auto used = inst->used();
                for(Size i = 0; i < used.size(); i++) {
                    auto v = a.base[used[i]];
                    if(isImplicit(v)) continue;
                    if(shape.uses[i].kind != ArgLocation::None) continue;

                    a.webFor(v).avoid |= mask;
                }

                // A return ends the function, so nothing can be live across it.
                if(!shape.isReturn) sites.push(ClobberSite { index, mask });
            }

            index++;
        };

        for(auto i: block->instructions.contents(a.base)) onInst(a.base[i]);
        onInst(a.base[block->terminator]);
    }

    for(auto& web: a.webs) {
        auto interval = web.interval();
        if(interval.isEmpty()) continue;

        for(auto& site: sites) {
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
 * "free", and both are weighted by loop depth, since an instruction one loop deep runs some multiple
 * of the times the code around it does. `orderBlocks` left that depth on each block for this.
 */

static constexpr U32 kReloadCost = 4;    // an operand that has to be brought into a register
static constexpr U32 kStoreCost = 4;     // a result that has to be carried back to its slot
static constexpr U32 kRematCost = 4;     // recreating the value where it is read
static constexpr U32 kFoldedUseCost = 1; // an operand read straight out of the frame

// How much more one execution of this block is worth than one execution of the function's entry.
// Eight per loop level, capped so that a deeply nested block cannot overflow the sums these feed.
static U32 blockWeight(LowerBlock* block) {
    auto depth = block->loopDepth < 5 ? block->loopDepth : 5;
    return U32(1) << (3 * depth);
}

// Whether this instruction reads and writes operand `index` in the same place - which it does when
// the encoding has such a form and the operand and the result turn out to be one web, so that they
// are certain to share whatever home that web gets. This is the shape phi-web coalescing produces
// for a loop-carried accumulator, and the one case where a spilled result costs nothing to store.
static bool isInPlace(Allocator& a, LowerInst* inst, I32 index) {
    if(index == kNoMemoryOperand || inst->createdCount == 0) return false;

    auto& result = inst->created()[0];
    auto operand = a.base[inst->used()[index]];

    if(isImplicit(&result) || isImplicit(operand)) return false;
    return a.sameWeb(operand, &result);
}

// The recipe that recreates `v`, if it is cheap and reproducible enough to have one. Every kind here
// is a constant in the sense that matters: it depends on nothing the program can write, so it
// produces the same answer wherever it is placed.
static bool recipeFor(Allocator& a, LowerValue* v, Remat& out) {
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

static void computeSpillCosts(Allocator& a) {
    // A recipe reproduces one definition, so only a web that has one can have a recipe. A web with
    // several members is several definitions of a single location, and no one of them describes it.
    Array<U16> members;
    for(Size i = 0; i < a.webs.size(); i++) members.push(0);
    for(auto web: a.webOf) members[web]++;

    for(Size i = 0; i < a.webOf.size(); i++) {
        auto& info = a.webs[a.webOf[i]];
        if(members[a.webOf[i]] != 1 || info.canRemat) continue;

        auto v = a.live.getValue(LiveId(i));
        if(isImplicit(v)) continue;

        info.canRemat = recipeFor(a, v, info.recipe);
    }

    for(auto offset: a.fun.blocks.contents(a.base)) {
        auto block = a.base[offset];
        auto weight = blockWeight(block);

        auto onInst = [&](LowerInst* inst) {
            auto shape = shapeOf(a.base, a.constraints, a.fun, inst);

            // The one operand this instruction could leave in the frame, if any. A read-modify-write
            // form takes precedence: it makes both the read and the write free, where a memory
            // source only makes the read free, and the two want the same r/m field.
            auto folded = memoryUseOperand(a.base, inst);
            auto inPlace = isInPlace(a, inst, memoryDefOperand(a.base, inst));
            if(inPlace) folded = memoryDefOperand(a.base, inst);

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
 * Pass 2: place values and record, per instruction, where the encoder finds each operand.
 */

struct Emitter {
    Allocator& a;
    LowerBase base;

    // Scratch registers handed out within the instruction currently being resolved, reset for each
    // one. A value whose home is a frame slot cannot be read by an encoder, so it is brought into
    // one of these first - and taken back to the frame afterwards if the instruction wrote it.
    Size tempsUsed[kPhysRegClassCount] = {};

    explicit Emitter(Allocator& a): a(a), base(a.base) {}

    RegId takeTemp(RegClass cls) {
        auto index = tempsUsed[cls]++;
        assertTrue(index < kMaxSpillTemps); // an instruction wanting more scratch than is reserved

        auto reg = spillTemp(cls, index);
        a.written.add(reg);
        return reg;
    }

    // Where the encoder reads operand `i`, given that the destructive destination (if any) has
    // already been placed. Used both to report operands and to keep a destructive result off the
    // registers its sibling operands are read from.
    //
    // `memoryDest` says that the result is being written straight into the frame slot operand zero
    // occupies, which takes the one r/m field this instruction has - so no *other* operand may stay
    // in memory, however good a form the encoding has for it.
    //
    // `reserve` is false when the caller only wants to know where a sibling operand will be read
    // from, so that asking twice does not consume two scratch registers for one operand.
    RegId useLocation(LowerInst* inst, const InstShape& shape, Size i, RegId destructiveReg, bool memoryDest, bool reserve) {
        auto v = base[inst->used()[i]];
        if(isImplicit(v)) return kInvalidReg;

        // A fixed-register operand is loaded straight into the register the instruction demands,
        // whether it comes from another register, from the frame or from a recipe - no scratch
        // needed in any of the three.
        auto want = wantForUse(shape, i);
        if(want != kInvalidReg) return want;
        if(i == 0 && destructiveReg != kInvalidReg) return destructiveReg;

        auto home = a.homeOf(v);
        if(isPhysicalLocation(home)) return home;

        // A slot this instruction can address directly stays where it is: the encoder takes the
        // memory form of the operation and the reload never exists.
        if(isSlot(home) && !memoryDest && memoryUseOperand(base, inst) == I32(i)) return home;

        auto cls = classForType(v->type);
        return reserve ? takeTemp(cls) : spillTemp(cls, tempsUsed[cls]);
    }

    // The register a freshly defined value would rather have: the one its source operand is about
    // to vacate, so that the copy the encoder would emit becomes `mov r, r` and disappears.
    RegId copyHint(LowerInst* inst, U32 index) {
        auto used = inst->used();
        if(used.size() == 0) return kInvalidReg;

        auto source = base[used[0]];
        if(isImplicit(source)) return kInvalidReg;

        auto& web = a.webFor(source);
        auto interval = web.interval();
        if(web.home == kInvalidReg || interval.isEmpty()) return kInvalidReg;

        // Only if the operand is genuinely finished with the register here: a value still live
        // after this instruction cannot hand its register to the result.
        if(interval.last() > afterInst(index)) return kInvalidReg;

        return web.home;
    }

    InstRegs resolveInst(LowerInst* inst, U32 index) {
        InstRegs out;
        Array<RegMove> pending;

        for(auto& used: tempsUsed) used = 0;

        auto shape = shapeOf(base, a.constraints, a.fun, inst);
        auto used = inst->used();
        auto created = inst->created();

        // The destructive destination has to be decided before anything else: it is where used()[0]
        // must sit by the time the instruction runs, so it is reported for both that operand and
        // the result. It must also avoid wherever the *other* operands are read from - the copy
        // that puts used()[0] there runs before the instruction, and would otherwise overwrite a
        // sibling operand that the instruction has not read yet.
        RegId destructiveReg = kInvalidReg;
        bool memoryDest = false;

        if(isDestructive(base, inst) && used.size() > 0 && created.size() > 0 && !isImplicit(&created[0])) {
            RegSet blocked;
            for(Size i = 1; i < used.size(); i++) {
                blocked.add(useLocation(inst, shape, i, kInvalidReg, false, false));
            }

            destructiveReg = a.assign(&created[0], blocked, copyHint(inst, index));

            if(isSlot(destructiveReg)) {
                // The result lives in the frame. Where the encoding has a form that writes its
                // destination through the r/m field and the operand it overwrites already occupies
                // that very slot, the whole operation happens in place - `add [rsp+8], rcx` - and
                // neither the reload nor the store exists. This is what a coalesced loop-carried
                // accumulator looks like once it has been spilled.
                auto memoryOperand = memoryDefOperand(base, inst);
                auto first = base[used[0]];

                memoryDest = memoryOperand == 0 && !isImplicit(first) && a.homeOf(first) == destructiveReg;

                // Otherwise it is computed in a scratch register and stored afterwards, and the
                // operand it overwrites has to be brought into that same one.
                if(!memoryDest) {
                    auto slot = destructiveReg;
                    destructiveReg = takeTemp(classForType(created[0].type));
                    out.postMoves.push(RegMove { destructiveReg, slot });
                }
            }
        }

        for(Size i = 0; i < used.size(); i++) {
            auto v = base[used[i]];
            auto location = useLocation(inst, shape, i, destructiveReg, memoryDest, true);

            out.uses.push(location);
            if(location != kInvalidReg && location != a.homeOf(v)) {
                pending.push(RegMove { a.homeOf(v), location });
            }
        }

        for(Size i = 0; i < created.size(); i++) {
            auto& v = created[i];

            if(isImplicit(&v)) {
                out.creates.push(kInvalidReg);
                continue;
            }

            if(i == 0 && destructiveReg != kInvalidReg) {
                out.creates.push(destructiveReg);
                continue;
            }

            auto want = wantForResult(shape, i);
            auto home = a.assign(&v, RegSet {}, want != kInvalidReg ? want : copyHint(inst, index));

            // Where the encoder has to write it, which is the home unless the home is a frame slot
            // this instruction has no destination form for, or the encoding forces a particular
            // register. A recipe stays a recipe: nothing is written anywhere, and the instruction
            // that would have defined the value emits nothing at all.
            auto at = home;
            if(want != kInvalidReg) at = want;
            else if(isSlot(home)) at = takeTemp(classForType(v.type));

            out.creates.push(at);

            // A result produced somewhere other than its home is carried there afterwards. For a
            // fixed register nothing live can be sitting in the way: it is part of this
            // instruction's written set, which every web crossing the instruction avoids.
            if(at != home) out.postMoves.push(RegMove { at, home });
        }

        sequenceMoves(pending, out.moves);
        return out;
    }

    // Places any phi in `successor` that hasn't been reached yet, and appends the copies carrying
    // this block's outgoing values into the phi registers.
    void resolvePhis(LowerBlock* block, LowerBlock* successor, U32 index, Array<RegMove>& pending) {
        for(auto p: successor->phis.contents(base)) {
            auto phi = base[p];
            auto& result = phi->result;
            if(isImplicit(&result)) continue;

            auto sources = phi->sources();
            auto incoming = phi->used();
            LowerValue* value = nullptr;

            for(Size i = 0; i < sources.size(); i++) {
                if(base[sources[i]] == block) { value = base[incoming[i]]; break; }
            }

            // Not an edge this phi takes a value from.
            if(!value || isImplicit(value)) continue;

            // The first edge to reach the phi decides its register, preferring the one the value
            // arriving over that edge is vacating - which coalesces the copy away entirely.
            // assign() only takes the hint if the register is genuinely free for the phi's whole
            // range, so offering it unconditionally is safe.
            // Coalesced: the two are one location and the transfer is an identity that never has
            // to be emitted. The web is placed by whichever of its members the walk reaches first.
            if(a.sameWeb(value, &result)) {
                a.assign(&result, RegSet {}, kInvalidReg);
                continue;
            }

            // Otherwise the first edge to reach the phi decides its register, preferring the one the
            // value arriving over that edge is vacating. assign() only takes the hint if the
            // register is free for the phi's whole interval, so offering it is always safe.
            a.assign(&result, RegSet {}, a.webFor(value).home);
            pending.push(RegMove { a.homeOf(value), a.homeOf(&result) });
        }
    }
};

// Places the incoming arguments and produces the copies, if any, that move them out of the places
// the calling convention delivered them in. An argument that outlives a call can't stay in a
// register the call clobbers, so it is given a safe one and copied there on entry - once, rather
// than being shuffled at every call site.
//
// Where each argument arrived is asked of the convention, not worked out here: the caller placed
// them from the same answer, so there is no second rule that could drift out of step with it.
static void assignArgs(Allocator& a, const CallConvention& convention, Array<RegMove>& entryMoves) {
    auto args = a.fun.args.contents(a.base);

    Array<ArgLocation> locations;
    classifyArgs(convention, args.size(), [&](Size i) {
        return a.base[args[i]]->result.type;
    }, locations);

    for(Size i = 0; i < args.size(); i++) {
        auto& result = a.base[args[i]]->result;
        if(isImplicit(&result)) continue;

        auto incoming = locations[i].reg;

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
            incoming = makeRegId(StackReg, slot);
        }

        // An argument nothing reads is dead the moment it arrives, so it gets no home. Giving it one
        // anyway would reserve a register for it across the entry copies - exactly where a function
        // with more arguments than it uses has the least to spare.
        if(result.uses.isEmpty()) continue;

        auto home = a.assign(&result, RegSet {}, incoming);
        if(home != incoming) entryMoves.push(RegMove { incoming, home });
    }
}

// Frame objects that come from the instructions rather than from the signature, plus the two facts
// about the stack that decide whether the function can address its frame through rsp.
static void collectFrameObjects(Allocator& a) {
    for(auto offset: a.fun.blocks.contents(a.base)) {
        for(auto i: a.base[offset]->instructions.contents(a.base)) {
            auto inst = a.base[i];

            if(inst->kind == LowerInst::Alloca) {
                auto count = a.base[((LowerInstAlloca*)inst)->byteCount];

                if(isImm(count)) {
                    // A compile-time size is an ordinary fixed frame object, and the alloca becomes
                    // an address computation rather than any change to the stack pointer.
                    auto size = ((LowerImm*)count->inst())->i;
                    assertTrue(size > 0 && size <= maxLimit<U32>);

                    auto slot = a.frame.add(StackSlot {
                        .kind = StackSlotKind::Local,
                        .slotClass = StackSlotClass::Slot64,
                        .size = U32(size),

                        // Nothing in the IR says what the allocation is going to hold, so it gets
                        // an alignment that suits any scalar or 128-bit vector stored into it.
                        .alignment = size >= 16 ? 16u : 8u,
                    });

                    a.frame.references.add(inst, FrameReference { .slot = slot });
                } else {
                    a.frame.hasDynamicAlloca = true;
                }
            }

            if(inst->kind == LowerInst::Call) {
                auto shape = shapeOf(a.base, a.constraints, a.fun, inst);
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

// One complete allocation of a function, with `reserved` held back from every value and `forceSpill`
// naming the webs a previous attempt asked to be left homeless. See allocateRegisters for what makes
// an attempt ask for another.
static FunctionRegs allocateWith(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live,
    const Constraints& constraints, RegSet reserved, bool framePointer,
    const Array<bool>& forceSpill, bool& needsScratch, Array<LiveId>& evicted)
{
    Allocator a(base, fun, live, constraints, reserved, forceSpill);
    collectFrameObjects(a);

    // Webs before avoid sets: a clobber that one member has to dodge is one the whole web has to
    // dodge, since they share a location. Costs after both, since a web is what carries them and an
    // in-place read-modify-write is a property of two values being one web.
    buildWebs(a);
    computeAvoidSets(a);
    computeSpillCosts(a);

    Emitter emitter(a);
    FunctionRegs result;

    // Arguments occupy their registers from the entry point, so they are placed before anything
    // else can claim one. Their entry copies belong to the first thing the function executes.
    Array<RegMove> entryMoves;
    assignArgs(a, constraints.getConvention(fun.callType), entryMoves);

    // The entry copies are emitted at index 0 below, which is only the first thing the function
    // executes because the implicit entry block holds no instructions - LowerFunction's constructor
    // creates it empty and nothing may branch to it, so its terminator is index 0. An entry block
    // with instructions would need them placed ahead of that instruction's own operand copies
    // instead.
    assertTrue(base[fun.blocks.get(base, 0)]->instructions.isEmpty());

    U32 index = 0;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        BlockRegs blockRegs;

        for(auto i: block->instructions.contents(base)) {
            blockRegs.insts.push(emitter.resolveInst(base[i], index));
            index++;
        }

        assertTrue(block->terminator != nullptr);
        auto terminatorRegs = emitter.resolveInst(base[block->terminator], index);

        // Phi copies run after whatever the terminator itself needs, and after the entry copies in
        // the entry block - a phi may be fed by an argument, which has to have reached its home
        // first. transformFunction guarantees that a block reaching any phi has a single successor,
        // so these copies cannot execute on a path that bypasses the phis.
        Array<RegMove> pending;
        for(auto successor: block->outgoing) {
            if(!successor) continue;

            assertTrue(base[successor]->phis.isEmpty() || !(block->outgoing[0] && block->outgoing[1]));
            emitter.resolvePhis(block, base[successor], index, pending);
        }

        if(index == 0) sequenceMoves(entryMoves, terminatorRegs.moves);
        sequenceMoves(pending, terminatorRegs.moves);

        blockRegs.insts.push(::move(terminatorRegs));
        index++;

        result.blocks.add(block, ::move(blockRegs));
    }

    assertTrue(index == live.instCount); // the walk here and buildRanges' numbering must agree

    for(Size i = 0; i < a.webOf.size(); i++) result.allocation.locations.push(a.webs[a.webOf[i]].home);
    result.frame = ::move(a.frame);
    result.remats = ::move(a.remats);
    result.framePointer = framePointer;

    // The decision was made before allocation started, so this is only a check that it was made
    // from the same facts the allocation then produced.
    assertTrue(!result.frame.hasDynamicAlloca || framePointer);

    // Which of the registers the function writes its caller expects to get back untouched. The
    // prologue saves exactly these and the epilogue restores them; a function that never left its
    // convention's clobber set saves nothing.
    result.usedCalleeSaved = a.written & constraints.getConvention(fun.callType).calleeSaved;
    assertTrue(result.usedCalleeSaved.classes[XmmReg] == 0); // no encoder saves a vector register yet

    needsScratch = a.needsScratch;
    evicted = ::move(a.evicted);
    return result;
}

// How many webs an allocation may displace across all of its attempts. Every displacement costs one
// more pass over the function, and the improvement from each is small and diminishing, so this
// bounds what a pathological function can spend. Reaching it costs code quality and nothing else:
// an attempt is a complete, correct allocation whether or not another one would have been better.
static constexpr Size kMaxEvictions = 16;

FunctionRegs allocateRegisters(Context& ctx, LowerBase base, LowerFunction& fun) {
    auto& constraints = targetConstraints();
    auto live = fun.buildLiveness(base);

    // Whether rbp is this function's frame pointer or one more register to hand out. Asked once,
    // here, and given to both the allocator and (through FunctionRegs) frame layout: the two
    // deciding it separately is the one way this can go wrong quietly, since a value placed in rbp
    // and a frame addressed through rbp are each individually correct.
    auto framePointer = functionNeedsFramePointer(ctx, base, fun);
    auto reserved = framePointer ? framePointerRegs() : RegSet {};

    Array<bool> forceSpill;
    for(Size i = 0; i < live->valueMap.size(); i++) forceSpill.push(false);

    /*
     * Every attempt is a complete allocation of the function, and the first is the answer for a
     * function that fitted in its registers - which is most of them, and which pays nothing for any
     * of what follows.
     *
     * Two things an attempt can discover that it cannot act on itself make it ask for another:
     *
     *   - a web ended up without a register, and one that has no register has to be brought into a
     *     scratch one at each instruction that touches it. Those are reserved for the whole
     *     function rather than found after the fact, so the function is allocated again with them
     *     held back. This can only happen once.
     *   - a web would rather have taken a register from a cheaper occupant than gone without one.
     *     The occupant cannot be moved where it stands, having already been emitted into the
     *     instructions that read it, so it is spilled from the start of the next attempt instead.
     *
     * Neither can fail to converge: the reserved set is monotone and settles after one change, and
     * the forced-spill set only grows and is bounded twice over - by kMaxEvictions and by there
     * being finitely many webs. Whatever no longer fits in a register goes to the frame, and the
     * frame has no limit.
     */
    FunctionRegs result;
    bool scratchReserved = false;
    Size evictions = 0;

    for(;;) {
        bool needsScratch = false;
        Array<LiveId> evicted;

        result = allocateWith(ctx, base, fun, *live, constraints, reserved, framePointer, forceSpill, needsScratch, evicted);
        bool again = false;

        if(needsScratch && !scratchReserved) {
            reserved |= spillTempRegs();
            scratchReserved = true;
            again = true;
        }

        for(auto id: evicted) {
            if(forceSpill[id] || evictions >= kMaxEvictions) continue;

            forceSpill[id] = true;
            evictions++;
            again = true;
        }

        if(!again) break;
    }

    // Debug builds only - assertTrue compiles away entirely in a release build, taking the call
    // with it. The verifier walks the whole function symbolically, which is too expensive to pay
    // for on every compile, and it can only ever fail on a bug in the code just above it.
    assertTrue(verifyAllocation(ctx, base, fun, *live, constraints, result));
    return result;
}
