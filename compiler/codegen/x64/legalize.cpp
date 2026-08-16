#include "gen.h"
#include "x64_util.h"

/*
 * Legalization.
 *
 * Placement decided where every value persists (place.cpp). This decides what that means at each
 * instruction: which location every operand is read from, where every result is written, and the
 * copies that bridge the difference between the two. Nothing here places anything - every location
 * it reports is either one placement chose, one the selected form demands, or a scratch register.
 *
 * That is the whole of the split. Placement answers "where does this value live", legalization
 * answers "where must it be at this instruction", and an answer to the second never changes an
 * answer to the first. The two used to be one walk, which is why displacing a web meant allocating
 * the function again: its location was already written into the instructions that read it.
 *
 * Five things a location cannot simply be handed to an instruction for, all of them handled by
 * *copying* around the awkward place rather than by moving a web's home:
 *
 *   - Fixed-register constraints (a divisor in rax, a call argument in rdi, ...). Operands are
 *     copied into place before the instruction and results copied out of place after it, so the
 *     web's home is unaffected. The copies are emitted as one parallel copy per instruction.
 *   - Clobbers. A web whose interval *crosses* a clobbering instruction was never given one of the
 *     clobbered registers, so there is nothing to rescue at the call.
 *   - Destructive two-address encodings, where the result overwrites its first operand's register.
 *     Placement puts the result where that operand can be copied to; the copy is emitted here.
 *   - A home in the frame. Most encoders cannot read one, so the value is loaded into a scratch
 *     register before the instruction and stored back after it if the instruction wrote it. Where
 *     the encoding does have a memory form, it is used and neither exists.
 *   - A home that is a recipe. Nothing holds the value at all, so it is recreated into a scratch
 *     register wherever it is read, and the instruction that would have defined it emits nothing.
 *
 * The scratch registers are reserved by placement being run again with them held back - see
 * TemporaryReserve and allocateRegisters - and how many of them a placement takes is measured by
 * running this same pass over it, which is what measureTemporaryReserve at the bottom is for.
 *
 * The result is checked before it is returned: verify.cpp simulates what the emitted code will leave
 * in each register and slot and confirms every instruction reads a location that actually holds the
 * value it wants. That runs in debug builds only, and it is the thing to reach for first when any of
 * this changes - it turns "wrong code in a shape nothing tests" into an assertion.
 */

/*
 * Parallel copies.
 */

// Whether some other transfer still to be emitted overwrites the source of transfer `i` - which is
// what makes `i` part of a cycle rather than something merely blocked by one.
static bool writesSource(const Array<RegMove>& pending, const IndexSet& done, Size i) {
    for(Size j = 0; j < pending.size(); j++) {
        if(j == i || done[j]) continue;
        if(pending[j].to == pending[i].from) return true;
    }

    return false;
}

// The scratch registers the move sequencer hands out, from the reserve held back for this function.
// A view rather than a copy so that the pass measuring the demand and the pass spending it go through
// one object: `used` is the measurement, and it is raised by the same call that returns the register.
struct MoveTemps {
    const TemporaryReserve& reserve;
    TemporaryReserve& used;
    RegSet& written;

    // Set while the demand is being measured, when the reserve is the widest one any instruction
    // could ask for rather than the one this function ended up with - see measureTemporaryReserve.
    bool measuring = false;

    MachineLocation take(RegisterClassId regClass, Size index) {
        auto bank = targetRegisters().regClass(regClass).bank;

        assertTrue(index < kMaxMoveTemps);                        // more than any sequence can want
        if(!measuring) assertTrue(index < reserve.moveTemps[bank]); // ... than the reserve holds back
        if(index + 1 > used.moveTemps[bank]) used.moveTemps[bank] = U8(index + 1);

        auto reg = reserve.moveTemp(bank, index);
        written.add(reg);
        return MachineLocation::physical(reg);
    }
};

// Sequences a set of simultaneous copies into an order that executes them one at a time without any
// of them destroying a value another still has to read. A copy can be emitted as soon as nothing
// left in the set reads its destination; when nothing qualifies, what remains is a permutation
// cycle, and it has to be broken.
//
// Two ways to break one. Where both ends are registers and the class has an exchange instruction -
// GPR `xchg` - it takes one instruction and needs nothing to go through, which is what makes
// cycle-breaking unable to fail for lack of a register. With a frame slot at either end, or in a
// class with no exchange at all, the destination is saved into a scratch register first and whoever
// was waiting to read it reads the scratch instead.
//
// A transfer with a slot at both ends - two spilled webs feeding the same phi - is expanded
// afterwards, since x86 has no memory-to-memory move. So is one out of a recipe and into a slot,
// for the same reason: the value has to exist in a register before anything can store it.
//
// The two want *different* registers only where both actually happen: the cycle's scratch holds a
// value the transfers after it still read, so an expansion sequenced in between cannot borrow it.
// Where a set has no cycle to break - which is nearly all of them - the expansion takes the pool's
// first register rather than its second, and the reserve measured for the function is one register
// smaller. That is a whole callee-saved register in `loopCallN`, whose parallel copy is eight
// stack-to-stack transfers and no cycle at all.
static void sequenceMoves(MoveTemps& temps, Array<RegMove>& pending, Array<RegMove>& out,
    IndexSet& done)
{
    done.reset(pending.size());
    for(Size i = 0; i < pending.size(); i++) done.set(i, pending[i].from == pending[i].to);

    auto begin = out.size();

    // Per bank, since the two pools are per bank: a cycle in the vector file says nothing about
    // which register a general-register expansion may use.
    bool brokeCycle[kRegisterBankCount] = {};

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
            done.set(i, true);
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
            done.set(i, true);

            MachineLocation reads;

            auto exchangeable = move.from.isPhysical() && move.to.isPhysical()
                && classHasExchange(move.regClass);

            if(exchangeable) {
                out.push(RegMove { move.from, move.to, move.regClass, true });
                reads = move.from;
            } else {
                // No exchange to reach for - a slot at one end, or a class the machine has no
                // exchange instruction for: park the destination somewhere first.
                brokeCycle[targetRegisters().regClass(move.regClass).bank] = true;
                auto scratch = temps.take(move.regClass, 0);
                out.push(RegMove { move.to, scratch, move.regClass });
                out.push(RegMove { move.from, move.to, move.regClass });
                reads = scratch;
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
        if(out[i].from.isPhysical() || !out[i].to.isStack()) continue;

        auto regClass = out[i].regClass;
        auto bank = targetRegisters().regClass(regClass).bank;
        auto scratch = temps.take(regClass, brokeCycle[bank] ? 1 : 0);
        auto to = out[i].to;

        out[i].to = scratch;
        out.insert(i + 1, RegMove { scratch, to, regClass });
        i++;
    }
}

/*
 * Split transitions.
 *
 * A web whose life was split occupies one location over most of it and another over each window -
 * see WebAllocation in gen.h. What that means here is one copy per segment boundary, and nothing
 * else: every location this pass reports still comes out of `locationOf`, which now answers
 * different things at different points.
 *
 * Where a boundary's copy goes follows from what a parallel copy means. The set of copies in front
 * of an instruction all read the state at its `before` point and the set behind it all read the
 * state at its `after` point, so a boundary belongs to whichever of the two straddles it: one at
 * `afterInst(i)` joins instruction i's `moves`, one at `beforeInst(i)` joins instruction i-1's
 * `postMoves`. Both are `(p - 1) / 2`, which is the rule below.
 *
 * Putting a reload in the *next* instruction's `moves` instead would be a copy that has to run
 * before its neighbours rather than beside them - an operand of that instruction reading the web
 * would be sequenced against a register the reload had not written yet - and a parallel copy has no
 * way to say that. This has none of it: every source in each set holds its value before any of them
 * runs, which is exactly what sequenceMoves assumes.
 *
 * A boundary never falls on a terminator, because a window may not cover one and verifyPlacement
 * checks that it does not. That is what lets the phi transfers at a terminator be sequenced as a
 * batch of their own below: they and a transition would have to be one simultaneous set, and they
 * never meet.
 */

// One boundary, and the copy that crosses it.
struct SegmentTransition {
    U32 index;    // the instruction it is attached to
    bool post;    // ... behind it rather than in front of it
    RegMove move;
};

static void collectTransitions(const Placement& placement, Array<SegmentTransition>& out) {
    for(Size w = 0; w < placement.webs.size(); w++) {
        auto& web = placement.webs[w];

        for(Size i = 0; i < web.segments.size(); i++) {
            auto& segment = web.segments[i];

            // A segment that names the home is no exception at all, and a region's boundaries are
            // copies on CFG edges rather than copies between two instructions - collectEdgeTransitions
            // owns those. Between them these two are what keeps the two walks from both claiming one
            // boundary.
            if(segment.location == web.home || !segment.inBlock()) continue;

            // Two segments of one location meeting at a point are one stretch, and neither end of the
            // join is a boundary: the copy out and the copy back in would be the same copy twice.
            // Nothing produces that today; skipping it is what keeps this walk total rather than
            // resting on that.
            auto joinedBefore = i > 0 && web.segments[i - 1].to == segment.from
                && web.segments[i - 1].location == segment.location;
            auto joinedAfter = i + 1 < web.segments.size() && web.segments[i + 1].from == segment.to
                && web.segments[i + 1].location == segment.location;

            if(!joinedBefore) {
                out.push(SegmentTransition {
                    .index = (segment.from - 1) / 2,
                    .post = (segment.from & 1) == 0,
                    .move = RegMove { web.home, segment.location, web.regClass },
                });
            }

            // Leaving a segment the web was *copied* into costs nothing: its home never stopped
            // holding the value, so the copy back would write what is already there. See
            // AllocationSegment::leavesFree, and §5.9 of place.cpp for why that is sound.
            if(!segment.leavesFree() && !joinedAfter) {
                out.push(SegmentTransition {
                    .index = (segment.to - 1) / 2,
                    .post = (segment.to & 1) == 0,
                    .move = RegMove { segment.location, web.home, web.regClass },
                });
            }
        }
    }

    // In the order the walk below will ask for them. Insertion sort because there are two of these
    // per split web and splitting is rare - most functions produce none at all.
    for(Size i = 1; i < out.size(); i++) {
        auto entry = out[i];
        auto j = i;

        while(j > 0 && out[j - 1].index > entry.index) {
            out[j] = out[j - 1];
            j--;
        }

        out[j] = entry;
    }
}

/*
 * Edge transitions.
 *
 * The other half of the same idea, for the boundaries an in-block segment cannot have. A web with a
 * Region segment is in one location inside the region's blocks and in its home outside them, so the
 * difference stands on the CFG edges into and out of the region rather than between two
 * instructions - and the location at a block's two ends is what those edges compare.
 *
 * Where the copy goes has exactly two answers, and which one applies is a property of the edge:
 *
 *   - **the predecessor has one successor.** Control reaching the end of it goes here and nowhere
 *     else, so the copy joins the terminator's batch - the same batch the phi transfers are
 *     sequenced in. They belong together: both describe the state at the successor's entry against
 *     the state at the predecessor's exit, so they are one simultaneous set and sequenceMoves is
 *     what orders them. A phi whose source web is itself being resolved here is then two reads of
 *     one source, which a parallel copy handles;
 *   - **otherwise the successor has one predecessor.** Control entering it came from here and
 *     nowhere else, so the copy goes at the start of the successor instead - BlockRegs::entryMoves,
 *     which exists for this. By splitPhiEdges' guarantee a successor with phis always takes the
 *     first case, so this batch never has to be sequenced against a phi transfer.
 *
 * An edge that is neither - a predecessor that branches into a successor that joins - has nowhere to
 * put a copy at all, and placement is what must not produce a difference on one. `verifyPlacement`
 * checks that independently; the assertion here is the second statement of it.
 *
 * Leaving a segment costs nothing wherever `leavesFree` says so, exactly as it does between two
 * instructions: the home never stopped holding the value. That is what makes a read-only promotion
 * one load on the way in and nothing at all on the way out.
 */

// The copies each block carries, by where they are emitted. Indexed by block index, which orderBlocks
// keeps equal to the block's position in the list.
struct EdgeTransitions {
    // Appended to the block's own terminator batch, beside its phi transfers.
    ArrayList<RegMove, 2> atExit;

    // Emitted before the block's first instruction - see BlockRegs::entryMoves.
    ArrayList<RegMove, 2> atEntry;

    // Set while any web has a region segment at all. Almost no function does, and the walk is skipped
    // outright where none has: it is a pass over every edge and every value live on it.
    bool any = false;

    void reset(Size blocks) {
        atExit.reset(blocks);
        atEntry.reset(blocks);
        any = false;
    }
};

static void collectEdgeTransitions(LowerBase base, LowerFunction& fun, Liveness& live,
    const Placement& placement, EdgeTransitions& out, Array<U32>& seen)
{
    out.reset(fun.blocks.size());

    for(Size w = 0; w < placement.webs.size(); w++) {
        for(auto& segment: placement.webs[w].segments) {
            if(!segment.inBlock()) { out.any = true; break; }
        }

        if(out.any) break;
    }

    if(!out.any) return;

    // Which webs this edge has already been answered for. A web may have several values live on one
    // edge - a phi coalesced with what feeds it is the ordinary case - and the question is about the
    // web. Stamped with an edge counter rather than cleared, since there are as many edges as blocks.
    seen.clear();
    for(Size i = 0; i < placement.webs.size(); i++) seen.push(0);
    U32 stamp = 0;

    for(auto offset: fun.blocks.contents(base)) {
        auto from = base[offset];
        auto fromSet = live.getBlock(from);
        auto exitPoint = afterInst(fromSet->lastIndex);

        // Both arms reaching one block is a single successor for this purpose: whichever way the
        // branch goes, control arrives there.
        auto singleSuccessor = !from->outgoing[0] || !from->outgoing[1]
            || from->outgoing[0] == from->outgoing[1];

        for(auto successorOffset: from->outgoing) {
            if(!successorOffset) continue;

            auto to = base[successorOffset];
            auto toSet = live.getBlock(to);
            auto entryPoint = beforeInst(toSet->firstIndex);
            stamp++;

            toSet->liveIn.iterate(toSet->valueCount, [&](Size raw) {
                auto id = LiveId(raw);

                // Live on *this* edge, which is what has to be carried across it. A value live out of
                // the predecessor on some other edge is not this edge's business.
                if(!fromSet->liveOut.get(fromSet->valueCount, raw)) return;

                auto webId = placement.webOf[id];
                if(seen[Size(webId)] == stamp) return;
                seen[Size(webId)] = stamp;

                auto& web = placement.webs[webId];
                MachineLocation at, want;
                if(!web.edgeTransfer(exitPoint, entryPoint, at, want)) return;

                if(singleSuccessor) {
                    out.atExit[from->index].push(RegMove { at, want, web.regClass });
                } else {
                    // The refusal placement is responsible for. A predecessor that branches into a
                    // successor that joins is a critical edge, and a copy on one has nowhere to go.
                    assertTrue(to->incoming.size() == 1);
                    out.atEntry[to->index].push(RegMove { at, want, web.regClass });
                }
            });
        }
    }
}

/*
 * Where one operand is read.
 *
 * Placement asks this too, about the operands of an instruction whose destructive result it is about
 * to place, so it lives here on its own rather than inside the walk below: the rule has to be one
 * rule, or the register placement keeps a result out of and the register the operand is read from
 * can drift apart.
 */
UseSite useSiteOf(LowerBase base, const MachineFunction& machine, const Placement& placement,
    LowerInst* inst, const InstShape& shape, Size i, U32 index, MachineLocation destructiveReg, bool memoryDest)
{
    auto v = base[inst->used()[i]];
    if(isImplicit(v)) return UseSite { MachineLocation::invalid() };

    // A fixed-register operand is loaded straight into the register the instruction demands,
    // whether it comes from another register, from the frame or from a recipe - no scratch needed
    // in any of the three.
    auto want = wantForUse(shape, i);
    if(want.isValid()) return UseSite { want };

    // The operand the result is written over reads from the result's own register, whichever operand
    // the form says that is - see the tie index below.
    if(I32(i) == machine.formOf(inst).tiedResult() && destructiveReg.isValid()) {
        return UseSite { destructiveReg };
    }

    auto home = placement.locationOf(v, beforeInst(index));
    assertTrue(home.isValid()); // an operand whose web placement never reached
    if(home.isPhysical()) return UseSite { home };

    // A slot this instruction can address directly stays where it is: the encoder takes the memory
    // form of the operation and the reload never exists. `memoryDest` says the result is being
    // written straight into the slot operand zero occupies, which takes the one r/m field this
    // instruction has - so no *other* operand may stay in memory, however good a form there is for
    // it.
    if(home.isStack() && !memoryDest && directMemoryOperands(base, machine, inst).read == I32(i)) {
        return UseSite { home };
    }

    return UseSite { MachineLocation::invalid(), true, bankForType(v->type) };
}

/*
 * Addresses.
 *
 * The one memory address an instruction references is resolved here, from the same placement every
 * other operand comes from - which is what leaves emission with an address object rather than a
 * pointer value it has to work out the shape of. Four things produce one:
 *
 *   - a folded X86Address, whose base and index were resolved at its own position just above the
 *     access that reads it;
 *   - a pointer the allocator left in a register, which is the degenerate `[reg]` case;
 *   - an outgoing argument store, at the offset in the argument area the convention assigned it;
 *   - a global's or a function's address, which is RIP-relative against a symbol whose offset is
 *     not known until everything has been emitted.
 *
 * A frame slot is deliberately not one of these: its address depends on a layout that has not run
 * yet, so it stays a location and the encoder builds the address from the frame.
 */

// The address an X86Address or X86Lea computes, with its operands resolved. The base and index each
// occupy one operand slot, in that order, and either may be absent.
static MachineAddress computedAddress(LowerBase base, LowerInstX86Address& addr, const Array<ResolvedOperand>& uses) {
    // A symbol is the whole address rather than a part of one: `[rip + g]` has no base, index or
    // scale field to combine it with, which is why the fold that builds one requires the other two
    // to be absent.
    if(addr.symbol) return MachineAddress::atSymbol(nullptr, base[addr.symbol]);

    MachineAddress out;
    Size operand = 0;

    auto physical = [&](Size i) {
        auto at = uses[i].at;
        assertTrue(at.isPhysical() && at.bank == BankGpr); // an address operand that is not a register
        return U8(at.index);
    };

    if(addr.hasBase) {
        out.hasBase = true;
        out.base = physical(operand++);
    }

    if(addr.hasIndex) {
        out.hasIndex = true;
        out.index = physical(operand++);
        out.scale = addr.scale;
    }

    out.displacement = I32(addr.displacement);
    return out;
}

/*
 * The walk.
 */

/*
 * The instruction record under construction: the same four lists an InstRegs holds, as arrays that
 * can be grown into. An instruction's operand count is known in advance but its copies are not - a
 * transfer appears because an operand turned out to be somewhere the encoding cannot read it - so
 * the record is built here and committed to the arena once its lengths are final.
 */
struct PendingRegs {
    Array<ResolvedOperand> uses;
    Array<ResolvedOperand> creates;

    MachineAddress address;
    bool hasAddress = false;

    Array<RegMove> moves;
    Array<RegMove> postMoves;

    void clear() {
        uses.clear();
        creates.clear();
        address = MachineAddress();
        hasAddress = false;
        moves.clear();
        postMoves.clear();
    }
};

/*
 * Everything legalization works in, held across the functions being allocated - see RegScratch.
 *
 * All of it is per-instruction or per-block state, which is the reason it is here: a buffer built
 * once per instruction is built tens of thousands of times to compile a small module, and every one
 * of them was a pair of allocations that the next instruction asked straight back for.
 */
struct LegalizeScratch {
    // One instruction's shape, asked for per instruction and emptied rather than rebuilt.
    ScratchPool<InstShape> shapes;

    // The copies that cross a split web's segment boundaries, in instruction order.
    Array<SegmentTransition> transitions;

    // ... and the ones that cross a CFG edge, per block. `edgeSeen` is the per-edge dedupe the walk
    // stamps.
    EdgeTransitions edges;
    Array<U32> edgeSeen;

    // The entry copies of the block being resolved: the edge's list copied into `edgeIn` so that the
    // sequencer takes it, and sequenced into `blockEntry` before being committed.
    Array<RegMove> edgeIn;
    Array<RegMove> blockEntry;

    // Where each folded address resolved to - see Legalizer::addresses.
    HashMap<LowerInst*, MachineAddress> addresses;

    // The record being built, and the two parallel copies feeding it.
    PendingRegs regs;
    Array<RegMove> pending;
    Array<RegMove> pendingPost;

    // The copies at the two places that are not an instruction's own: the entry, and a block's
    // outgoing phi transfers. Both are sequenced into the terminator's record.
    Array<RegMove> entryMoves;
    Array<RegMove> phiMoves;

    // The copies §7.2.3 takes out of both arms of one branch, before they are committed.
    Array<RegMove> hoisted;

    // Which of the transfers sequenceMoves has already emitted. One buffer serves the whole pass:
    // sequencing a parallel copy never sequences another.
    IndexSet done;
};

void destroyLegalizeScratch(LegalizeScratch* scratch) {
    delete scratch;
}

struct Legalizer {
    LowerBase base;
    LowerFunction& fun;
    const MachineFunction& machine;
    const Constraints& constraints;
    const Placement& placement;

    LegalizeScratch& scratch;

    // Where the instruction records are written - see commitSlice. Not rewound here: what this pass
    // produces is exactly what outlives it.
    RecordArena& records;

    // One instruction's shape, asked for per instruction and emptied rather than rebuilt.
    ScratchPool<InstShape>& shapes;

    // The scratch registers held back for this function, which this pass hands out from - see
    // TemporaryReserve. Held by value because the measuring pass runs against the widest one rather
    // than against whatever this function ended up with.
    TemporaryReserve reserve;

    // What this pass actually asked for, which is the measurement measureTemporaryReserve returns and
    // which allocateRegisters holds back on the next placement pass.
    TemporaryReserve used;

    // Set while the demand is being measured rather than spent, in which case running past the
    // reserve is the answer being looked for rather than a failure.
    bool measuring = false;

    // The scratch registers this pass handed out, which the function has to save if any of them is
    // callee-saved. Placement counts the registers it gave to webs; these are the other half.
    RegSet written;

    // The address each folded X86Address resolved to, so that the access it belongs to can name it
    // rather than reconstructing it. Keyed by instruction because an address is placed immediately
    // in front of its user and resolved just before it. The scratch's, emptied per function.
    HashMap<LowerInst*, MachineAddress>& addresses;

    // Scratch registers handed out within the instruction currently being resolved, reset for each
    // one. A value whose home is a frame slot cannot be read by an encoder, so it is brought into
    // one of these first - and taken back to the frame afterwards if the instruction wrote it.
    Size tempsUsed[kRegisterBankCount] = {};

    /*
     * The registers a folded address is already holding at the instruction that folds it.
     *
     * An X86Address emits nothing and is resolved at its own position, one instruction in front of
     * the access that reads it - so its base and its index are *live into* that access, in whatever
     * locations they were resolved to. When either of them came from a frame slot or a recipe, that
     * location is a scratch register, and `tempsUsed` resetting per instruction would hand the very
     * same one to an operand of the access. The address is then read through a register holding the
     * value that was about to be stored into it.
     *
     * This is what that reset has to know about. The registers are excluded rather than the pool
     * being offset, because what has to be avoided is a *register* - and a folded address whose
     * operands are ordinary registers blocks nothing, since a web is never placed in the reserve.
     *
     * Kept as a set rather than as a count because the two operands may come from different pools:
     * an index in a scratch register and a base in a real one is the common case, and skipping two
     * positions for it would grow the reserve for nothing.
     */
    RegSet foldedAddressRegs;

    /*
     * And the registers the instruction's own expansion is going to write.
     *
     * The same hazard as the paragraph above, reached from the other side and found by the first
     * wide vector that had to be reloaded at a pseudo. A form that expands into several machine
     * instructions needs a register that is neither operand nor result, and it says so by declaring
     * a **clobber** - `FormVSelect` names xmm15, the lane accesses name it, the negation names it.
     * A clobber keeps a live *web* out of that register at this instruction, which is what it is
     * for; what it does not do, and could not, is keep the *scratch pool* out of it, because the
     * pool is not a web and is handed out here rather than by placement.
     *
     * And the pool is taken from the top of each register file, which is exactly where a form
     * looking for a register nothing else wants also reached. So an operand that had to come out of
     * a frame slot at one of those instructions was reloaded into xmm15 and then overwritten by the
     * expansion that was told xmm15 was free:
     *
     *     vmovups (%rsp), %ymm15                  # the operand, reloaded into the operand temp
     *     vextracti128 $0, %ymm15, %xmm15         # the expansion's scratch, same register
     *     vinserti128 $0, %xmm15, %ymm15, %ymm0   # reads %ymm15 for the half it did not write
     *
     * which answered a vector whose upper half was zero. Nothing in the suite spilled a vector
     * before the 256-bit tier, so this had never fired - the general-register pseudos avoid it by
     * accident rather than by rule, `FormImmFloat32` using r11 where the pool starts at r15.
     *
     * Excluded rather than counted, for the reason above: what has to be avoided is a *register*,
     * and a form that clobbers one the pool would not have reached blocks nothing.
     */
    RegSet formClobberRegs;

    // The copies that cross a split web's segment boundaries, in instruction order, and how far the
    // walk has read into them. One cursor serves the function because the walk visits every
    // instruction index once and in order.
    Array<SegmentTransition>& transitions;
    Size transitionCursor = 0;

    // Liveness, for the one question this pass asks of it: which webs are live on an edge, and so
    // which of them an edge transition has to carry. Everything else here is answered by the
    // placement.
    Liveness& live;

    // The copies on the CFG edges, collected once for the function - see collectEdgeTransitions.
    EdgeTransitions& edges;

    Legalizer(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
        const Constraints& constraints, const Placement& placement, const TemporaryReserve& reserve,
        RegScratch& regScratch):
        base(base), fun(fun), machine(machine), constraints(constraints), placement(placement),
        scratch(*regScratch.legalize), records(regScratch.records), shapes(regScratch.legalize->shapes),
        reserve(reserve), addresses(regScratch.legalize->addresses),
        transitions(regScratch.legalize->transitions), live(live), edges(regScratch.legalize->edges)
    {
        addresses.reset();
        transitions.clear();
        collectTransitions(placement, transitions);
        collectEdgeTransitions(base, fun, live, placement, edges, regScratch.legalize->edgeSeen);
    }

    // The record just resolved, copied into the arena at the length it turned out to be. Everything
    // the walk builds is in one set of buffers, so this has to be called before the next instruction
    // is resolved over them.
    InstRegs commit() {
        auto& regs = scratch.regs;

        return InstRegs {
            commitSlice(records, regs.uses),
            commitSlice(records, regs.creates),
            regs.address,
            regs.hasAddress,
            commitSlice(records, regs.moves),
            commitSlice(records, regs.postMoves),
        };
    }

    // The boundaries this instruction carries, added to the parallel copy on the side each falls.
    void appendTransitions(U32 index, Array<RegMove>& pending, Array<RegMove>& pendingPost) {
        while(transitionCursor < transitions.size() && transitions[transitionCursor].index == index) {
            auto& transition = transitions[transitionCursor++];
            (transition.post ? pendingPost : pending).push(transition.move);
        }

        // Every index the walk passes is asked for, so nothing may be left behind one.
        assertTrue(transitionCursor == transitions.size() || transitions[transitionCursor].index > index);
    }

    // The scratch registers the move sequencer draws on, over the same reserve and the same
    // measurement this instruction's operands use.
    MoveTemps moveTemps() { return MoveTemps { reserve, used, written, measuring }; }

    MachineLocation takeTemp(RegisterBankId bank) {
        auto index = tempsUsed[bank]++;

        // Past anything the address this instruction folds is still holding - see
        // foldedAddressRegs. The skipped positions are consumed rather than stepped over, so the
        // measurement below reports the demand this instruction really has and the reserve grows to
        // cover it on the next placement round.
        while(index < kMaxOperandTemps
              && (foldedAddressRegs.has(reserve.operandTemp(bank, index))
                  || formClobberRegs.has(reserve.operandTemp(bank, index))))
        {
            index = tempsUsed[bank]++;
        }

        assertTrue(index < kMaxOperandTemps);                          // more than any form can want
        if(!measuring) assertTrue(index < reserve.operandTemps[bank]);  // ... than the reserve holds back
        if(index + 1 > used.operandTemps[bank]) used.operandTemps[bank] = U8(index + 1);

        auto reg = reserve.operandTemp(bank, index);
        written.add(reg);
        return MachineLocation::physical(reg);
    }

    // The registers the address `inst` folds is already holding, or nothing where it folds none.
    // Asked before any of this instruction's own temporaries are handed out, since that is the whole
    // point - see foldedAddressRegs.
    RegSet regsHeldByFoldedAddress(LowerInst* inst) {
        RegSet held;

        auto operand = machine.formOf(inst).addressOperand();
        if(operand < 0) return held;

        auto value = base[inst->used()[operand]];
        if(!isMem(value)) return held;

        auto found = addresses.getValue(value->inst());
        if(!found) return held;

        auto address = found.unwrap();
        if(address.hasBase) held.add(PhysicalReg { BankGpr, U16(address.base) });
        if(address.hasIndex) held.add(PhysicalReg { BankGpr, U16(address.index) });

        return held;
    }

    // Where a value lives at the instruction numbered `index`, which by this point is settled:
    // placement ran to completion before any of this did. One query serves an instruction's operands
    // and its results alike, since a web occupies one location for the whole of an instruction - a
    // split that ended a segment in the middle of one would have nowhere legal to put the transfer.
    //
    // A *point* query and not the web's home, which are two different questions the moment a segment
    // can differ at a block boundary. It was called `homeOf` while the two agreed, and what that hid
    // is in `resolvePhis` below.
    MachineLocation locationAt(LowerValue* v, U32 index) {
        auto at = placement.locationOf(v, beforeInst(index));
        assertTrue(at.isValid()); // a value placement never reached
        return at;
    }

    // Where the encoder reads operand `i`, given that the destructive destination (if any) has
    // already been resolved.
    //
    // `take` is false when the caller only wants to know where a sibling operand will be read from,
    // so that asking twice does not consume two scratch registers for one operand.
    MachineLocation useLocation(LowerInst* inst, const InstShape& shape, Size i, U32 index,
        MachineLocation destructiveReg, bool memoryDest, bool take)
    {
        auto site = useSiteOf(base, machine, placement, inst, shape, i, index, destructiveReg, memoryDest);
        if(!site.needsTemp) return site.at;

        return take
            ? takeTemp(site.tempBank)
            : MachineLocation::physical(reserve.operandTemp(site.tempBank, tempsUsed[site.tempBank]));
    }

    // The address of a memory operand: a folded X86Address resolved at its own position just above
    // this instruction, or a pointer the allocator left in a register.
    MachineAddress operandAddress(LowerValue* value, const ResolvedOperand& direct) {
        if(isMem(value)) {
            auto found = addresses.getValue(value->inst());
            assertTrue(found.isJust()); // an addressing mode its user was resolved before
            return found.unwrap();
        }

        // A global whose address never had to exist: `[rip + g]` is what the access is emitted as,
        // so the symbol is the address rather than something copied into a register first. The
        // peephole in transform.cpp is what decided that, and left the value with no location at
        // all - which is why this is asked here rather than of the placement.
        if(value->inst()->kind == LowerInst::Global && isImplicit(value)) {
            return MachineAddress::atSymbol(nullptr, base[((LowerInstGlobal*)value->inst())->target]);
        }

        auto at = direct.at;
        assertTrue(at.isPhysical() && at.bank == BankGpr); // a pointer operand that is not a register
        return MachineAddress::atRegister(U8(at.index));
    }

    // The one memory address this instruction references, if its encoding has an address field at
    // all - see the block comment above.
    void resolveAddress(LowerInst* inst, PendingRegs& out) {
        auto set = [&](MachineAddress address) {
            out.address = address;
            out.hasAddress = true;
        };

        // An instruction that *references* memory says so by naming an address operand in its form,
        // and every one of them resolves the same way: a folded X86Address, or a pointer left in a
        // register. Which operand it is comes from the form, so a load, a store and a cache-control
        // intrinsic need no case of their own here.
        auto operand = machine.formOf(inst).addressOperand();
        if(operand >= 0) {
            set(operandAddress(base[inst->used()[operand]], out.uses[operand]));
            return;
        }

        // What is left are the instructions that *compute* an address rather than reference one,
        // and the two that name a place nothing in the IR points at.
        switch(inst->kind) {
            case LowerInst::X86Address:
                // Emits nothing itself: it is resolved so that whichever access folds it in can name
                // the answer rather than working it out again.
                addresses.add(inst, computedAddress(base, *(LowerInstX86Address*)inst, out.uses));
                break;

            case LowerInst::X86Lea:
                set(computedAddress(base, *(LowerInstX86Address*)inst, out.uses));
                break;

            case LowerInst::X86PushArg:
                // The outgoing argument area is always addressed through rsp: it is the lowest part
                // of the frame and reserved once by the prologue, so it stays where the callee looks
                // for it whatever else the function does to its stack.
                set(MachineAddress::atOffset(U8(IntRegister::rsp), I32(((LowerInstX86PushArg*)inst)->stackOffset)));
                break;

            case LowerInst::Global:
                // Folded into every access that reads it, in which case there is no `lea` here and
                // nothing to resolve - see operandAddress above and tryFoldGlobalAddress.
                if(!isImplicit(&((LowerInstGlobal*)inst)->result)) {
                    set(MachineAddress::atSymbol(nullptr, base[((LowerInstGlobal*)inst)->target]));
                }
                break;

            case LowerInst::Fun:
                // Elided when every use is a direct call, which encodes the target as a rel32 and
                // never reads the address out of a register.
                if(!isImplicit(&((LowerInstFun*)inst)->result)) {
                    set(MachineAddress::atSymbol(base[((LowerInstFun*)inst)->target], nullptr));
                }
                break;

            default:
                break;
        }
    }

    // Resolves one instruction into `scratch.regs`, ready for `commit`. It writes rather than
    // returns because the terminator's record takes one more set of copies after this has run - the
    // phi transfers on its outgoing edges - and a record already committed to the arena cannot grow.
    void resolveInst(LowerInst* inst, U32 index) {
        auto& out = scratch.regs;
        out.clear();

        // The two parallel copies this instruction needs: the transfers that put its operands where
        // it reads them, and the ones that carry its results from where it writes them to their
        // homes. Both are *simultaneous* sets rather than sequences - an instruction with two
        // results in fixed registers can perfectly well have the first one's home be the second
        // one's register - so both are sequenced before they are emitted.
        auto& pending = scratch.pending;
        auto& pendingPost = scratch.pendingPost;
        pending.clear();
        pendingPost.clear();

        for(auto& used: tempsUsed) used = 0;
        foldedAddressRegs = regsHeldByFoldedAddress(inst);

        // A call's clobber set is the callee's and says nothing about registers the *caller* may
        // read its operands out of, so only a form's own declared clobbers are in the way here -
        // which is the same distinction `ClobberSite::operandMask` draws one pass earlier.
        formClobberRegs = machine.formOf(inst).clobbers;

        Scratch<InstShape> held(shapes);
        auto& shape = *held;
        shapeOf(base, machine, constraints, fun, inst, shape);
        auto used = inst->used();
        auto created = inst->created();

        // The destructive destination has to be resolved before anything else: it is where the tied
        // operand must sit by the time the instruction runs, so it is reported for both that operand
        // and the result. Placement already kept it off the registers the *other* operands are read
        // from, which is what makes the copy that puts it there safe to emit in front of the
        // instruction.
        MachineLocation destructiveReg;
        bool memoryDest = false;

        // Which operand the result is written over, if any. Almost every form here ties operand
        // zero - two-address arithmetic reads and writes its first source - but `pblendvb` preserves
        // its *second* and takes bytes from its first, so the index is read rather than assumed.
        auto tied = machine.formOf(inst).tiedResult();

        if(tied >= 0 && Size(tied) < used.size() && created.size() > 0 && !isImplicit(&created[0])) {
            destructiveReg = locationAt(&created[0], index);

            if(destructiveReg.isStack()) {
                // The result lives in the frame. Where the encoding has a form that writes its
                // destination through the r/m field and the operand it overwrites already occupies
                // that very slot, the whole operation happens in place - `add [rsp+8], rcx` - and
                // neither the reload nor the store exists. This is what a coalesced loop-carried
                // accumulator looks like once it has been spilled.
                auto choice = directMemoryOperands(base, machine, inst);
                auto operandHome = choice.hasReadWrite()
                    ? locationAt(base[used[choice.readWrite]], index)
                    : MachineLocation::invalid();

                memoryDest = takesInPlace(choice, operandHome, destructiveReg);

                // Otherwise it is computed in a scratch register and stored afterwards, and the
                // operand it overwrites has to be brought into that same one.
                if(!memoryDest) {
                    auto slot = destructiveReg;
                    destructiveReg = takeTemp(bankForType(created[0].type));
                    pendingPost.push(RegMove { destructiveReg, slot, classForType(created[0].type) });
                }
            }
        }

        for(Size i = 0; i < used.size(); i++) {
            auto v = base[used[i]];

            // An operand the encoding carries as a constant occupies no location at all. Its value
            // is resolved here so that emission reads it from the operand record rather than
            // reaching back into the IR to find out that this operand was an immediate.
            if(isImm(v)) {
                out.uses.push(ResolvedOperand::constant(((LowerImm*)v->inst())->i));
                continue;
            }

            auto location = useLocation(inst, shape, i, index, destructiveReg, memoryDest, true);
            auto regClass = classForType(v->type);

            out.uses.push(ResolvedOperand::location(location, regClass));
            if(location.isValid() && location != locationAt(v, index)) {
                pending.push(RegMove { locationAt(v, index), location, regClass });
            }
        }

        for(Size i = 0; i < created.size(); i++) {
            auto& v = created[i];
            auto regClass = classForType(v.type);

            if(isImplicit(&v)) {
                out.creates.push(ResolvedOperand::none());
                continue;
            }

            if(i == 0 && destructiveReg.isValid()) {
                out.creates.push(ResolvedOperand::location(destructiveReg, regClass));
                continue;
            }

            auto want = wantForResult(shape, i);

            // Not `locationAt`: a result nothing reads has no home at all (markReadWebs in
            // place.cpp), which is the one case where an invalid answer here is not a placement
            // that failed to reach something. Placement only leaves one homeless where the form
            // names the register it is produced in, so `want` below is what answers for it and
            // there is nothing to carry anywhere afterwards - which is the whole of the saving: a
            // call whose return value is discarded leaves it in rax and emits nothing.
            auto home = placement.locationOf(&v, beforeInst(index));

            // Where the encoder has to write it, which is the home unless the home is a frame slot
            // this instruction has no destination form for, or the encoding forces a particular
            // register. A recipe stays a recipe: nothing is written anywhere, and the instruction
            // that would have defined the value emits nothing at all.
            auto at = home;
            if(want.isValid()) at = want;
            else if(home.isStack()) at = takeTemp(bankForType(v.type));

            assertTrue(at.isValid()); // a result with neither a home nor a register the form names
            out.creates.push(ResolvedOperand::location(at, regClass));

            // A result produced somewhere other than its home is carried there afterwards. For a
            // fixed register nothing live can be sitting in the way: it is part of this
            // instruction's written set, which every web crossing the instruction avoids.
            if(home.isValid() && at != home) pendingPost.push(RegMove { at, home, regClass });
        }

        // A constant materialization carries the value it defines rather than an operand of its
        // own, which is what the form's immediate field naming a result says.
        auto& immField = machine.formOf(inst).encoding.immField;
        if(!immField.isNone() && immField.result) {
            assertTrue(inst->kind == LowerInst::Imm); // a form defining a constant that is not one
            out.creates[immField.index].immediate = ((LowerImm*)inst)->i;
            out.creates[immField.index].isImmediate = true;
        }

        // The segment boundaries that fall here, which join the two sets rather than forming a third
        // - a boundary and an operand copy are simultaneous, both reading the same state.
        appendTransitions(index, pending, pendingPost);

        resolveAddress(inst, out);
        auto temps = moveTemps();
        sequenceMoves(temps, pending, out.moves, scratch.done);
        sequenceMoves(temps, pendingPost, out.postMoves, scratch.done);
    }

    // The copies carrying this block's outgoing values into a successor's phi locations. A phi that
    // shares a web with the value arriving over this edge is already where it needs to be, and the
    // transfer is an identity that sequenceMoves drops.
    //
    // **The two ends are read at two different points**, and that is not a nicety. The value leaves
    // from wherever it is at the *predecessor's terminator*; the phi has to arrive wherever it is at
    // the *successor's entry*. Those were the same number for as long as a web had one location at
    // every boundary, and a single query answered both - which is what hid this. A region segment
    // covering the successor and not the predecessor makes them differ, and asking the predecessor's
    // point for both would copy the value into where the phi lives everywhere *except* in the block
    // that reads it.
    void resolvePhis(LowerBlock* block, LowerBlock* successor, U32 index, Array<RegMove>& pending) {
        auto entryPoint = beforeInst(live.getBlock(successor)->firstIndex);

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

            auto from = locationAt(value, index);
            auto to = placement.locationOf(&result, entryPoint);
            assertTrue(to.isValid()); // a phi placement never reached
            if(from != to) pending.push(RegMove { from, to, classForType(result.type) });
        }
    }

    // The copies that move the incoming arguments out of the places the calling convention delivered
    // them in. Where each arrived is placement's record of it, so the frame object a stack argument
    // came in is the one placement created rather than one found again by searching for it.
    void resolveArgs(Array<RegMove>& entryMoves) {
        auto args = fun.args.contents(base);

        for(Size i = 0; i < args.size(); i++) {
            auto& result = base[args[i]]->result;
            if(isImplicit(&result)) continue;

            auto incoming = placement.incomingArgs[i];
            auto home = placement.locationOf(&result, beforeInst(0));

            // An argument nothing reads was never given a home: there is nothing to carry it to.
            if(!home.isValid()) continue;
            if(home != incoming) entryMoves.push(RegMove { incoming, home, classForType(result.type) });
        }
    }
};

/*
 * §7.2.3 Two arms that begin the same way.
 *
 * A conditional branch whose successors both start with the same copy has written that copy twice:
 *
 *     jle  .body                 vmovaps %ymm0, %ymm2
 *     vmovaps %ymm0, %ymm2  ->    jle  .body
 *     jmp  .join                 jmp  .join
 *   .body:                     .body:
 *     vmovaps %ymm0, %ymm2       ...
 *     ...
 *
 * The shape is not exotic and is not a failure of coalescing: the arms of a rotated loop's guard
 * both establish the accumulator's register - one for the loop body's first iteration, the other for
 * the phi that skips it - and the two copies are the same copy because the value and its home are
 * the same on both paths. `productVectors` in `test/resolve/VecBulk.yana` is where it was found, and
 * every bulk operation in that file has one.
 *
 * ## What makes it sound
 *
 * The copy is emitted at the block's *exit* - after the terminator's own operand copies, before the
 * branch - which is the one program point common to both paths and later than everything either
 * block's own copies establish. Four conditions, and each closes a way the motion could lose
 * something:
 *
 *  - **Both successors have this block as their only predecessor.** A copy taken out of a block
 *    reached from anywhere else would stop happening on that other path.
 *  - **The destination is not something the terminator names.** The copy now runs in front of the
 *    branch rather than behind it, so a register the branch reads - its condition, or a base of an
 *    address it folds - would be overwritten before it was read. The terminator's own copies are
 *    checked as well, at both ends: they run first, and one whose destination this overwrote would
 *    have been made for nothing.
 *  - **Register to register, in one class.** Two reasons, and the second is the load-bearing one. A
 *    copy through the frame or out of a recipe may need a scratch register, and the two sequences it
 *    would be spliced between have each already been given theirs. And the copy lands between the
 *    block's *comparison* and the branch that reads its flags - a block ending in a conditional
 *    branch nearly always holds one - so it may not write them: a register copy is a `mov` or a
 *    `movaps` and writes none, where a recipe materialization can be anything the recipe is.
 *  - **Nothing follows the terminator.** A result carried out of a fixed register (`postMoves`) is
 *    emitted after the branch has been taken, which is not a place anything on both paths can be.
 *
 * The source needs no condition at all. Every hoisted copy stands after this block's own copies and
 * in the order the two arms had them, so what each reads is exactly what it read where it was.
 *
 * ## Where it runs
 *
 * On the records, after the walk that built them and before anything reads them - so a block left
 * with nothing to emit is a block `computeBypass` can take out, which is the second half of the
 * saving where an arm was one copy and a jump.
 */
static bool sameCopy(const RegMove& a, const RegMove& b) {
    if(a.swap || b.swap) return false;
    if(a.regClass != b.regClass) return false;
    if(!a.from.isPhysical() || !a.to.isPhysical()) return false;

    return a.from == b.from && a.to == b.to;
}

// Every register the terminator's record names, at either end of its copies and in its own operands,
// results and folded address. A hoisted copy stands between the copies and the instruction, so a
// destination anywhere in here is one it would destroy.
static bool terminatorNames(const InstRegs& term, MachineLocation at) {
    for(auto& move: term.moves) {
        if(move.from == at || move.to == at) return true;
    }

    for(auto& use: term.uses) {
        if(use.at == at) return true;
    }

    for(auto& create: term.creates) {
        if(create.at == at) return true;
    }

    if(term.hasAddress && at.isPhysical() && at.bank == BankGpr) {
        auto& address = term.address;
        if(address.hasBase && address.base == at.index) return true;
        if(address.hasIndex && address.index == at.index) return true;
    }

    return false;
}

/*
 * The copies an arm runs before anything else, and where they are kept.
 *
 * Two lists can hold them and the difference is where the arm came from. A block reached over an
 * edge that carried a location change has them in its `entryMoves`; a block that *is* the edge - the
 * split block a critical edge became, whose whole content is a phi transfer and a jump - has them in
 * its terminator's `moves`. The rotated loop guard that motivated this produces one of each: the arm
 * entering the body and the arm skipping it are both edge blocks.
 *
 * Nothing else qualifies. A prefix taken out of the copies in front of a real instruction would have
 * to reason about `emitAsLea`, which reads that list and folds it into the instruction behind it;
 * and a block whose terminator is itself a branch has a comparison standing between its copies and
 * its exit.
 */
struct ArmPrefix {
    SmallBuffer<RegMove>* moves = nullptr;
};

static ArmPrefix armPrefixOf(LowerBase base, BlockRegs& regs, LowerBlock* block) {
    if(regs.entryMoves.size() != 0) return ArmPrefix { &regs.entryMoves };
    if(block->instructions.isNotEmpty()) return ArmPrefix {};
    if(base[block->terminator]->kind != LowerInst::Jmp) return ArmPrefix {};

    // The terminator's record is the only one a block with no instructions has, and `postMoves`
    // there would be something running behind the jump.
    auto& term = regs.insts[0];
    if(term.postMoves.size() != 0) return ArmPrefix {};

    return ArmPrefix { &term.moves };
}

static void hoistCommonEntryMoves(Legalizer& l, LowerBase base, LowerFunction& fun, LegalizedFunction& result) {
    auto& hoisted = l.scratch.hoisted;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        if(!block->outgoing[0] || !block->outgoing[1]) continue;

        auto left = base[block->outgoing[0]];
        auto right = base[block->outgoing[1]];
        if(left == right) continue;
        if(left->incoming.size() != 1 || right->incoming.size() != 1) continue;

        auto here = result.blocks.get(block);
        auto atLeft = result.blocks.get(left);
        auto atRight = result.blocks.get(right);
        if(!here.isJust() || !atLeft.isJust() || !atRight.isJust()) continue;

        auto& blockRegs = here.unwrap();
        auto& term = blockRegs.insts[blockRegs.insts.size() - 1];
        if(term.postMoves.size() != 0) continue;

        auto leftArm = armPrefixOf(base, atLeft.unwrap(), left);
        auto rightArm = armPrefixOf(base, atRight.unwrap(), right);
        if(!leftArm.moves || !rightArm.moves) continue;

        hoisted.clear();
        Size taken = 0;

        while(taken < leftArm.moves->size() && taken < rightArm.moves->size()) {
            auto& move = (*leftArm.moves)[taken];
            if(!sameCopy(move, (*rightArm.moves)[taken])) break;
            if(terminatorNames(term, move.to)) break;

            hoisted.push(move);
            taken++;
        }

        if(hoisted.isEmpty()) continue;

        auto drop = [&](SmallBuffer<RegMove>& moves) {
            moves = SmallBuffer<RegMove> { moves.data() + taken, moves.size() - taken };
        };

        drop(*leftArm.moves);
        drop(*rightArm.moves);
        blockRegs.exitMoves = commitSlice(l.records, hoisted);
    }
}

// The one walk, run either to produce the instruction records or to measure what producing them
// costs in scratch registers - see measureTemporaryReserve. The two are the same pass because a
// separate rule for the demand would be a second answer to one question, and the one that is wrong
// is the one that leaves an instruction with nowhere to bring a spilled operand.
static void runLegalizer(Legalizer& l, LowerBase base, LowerFunction& fun, LegalizedFunction& result) {
    result.clear();

    // The entry copies are emitted at index 0 below, which is only the first thing the function
    // executes because the implicit entry block holds no instructions - LowerFunction's constructor
    // creates it empty and nothing may branch to it, so its terminator is index 0. An entry block
    // with instructions would need them placed ahead of that instruction's own operand copies
    // instead.
    assertTrue(base[fun.blocks.get(base, 0)]->instructions.isEmpty());

    auto& entryMoves = l.scratch.entryMoves;
    entryMoves.clear();
    l.resolveArgs(entryMoves);

    U32 index = 0;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        BlockRegs blockRegs;

        // The edge copies that had to go at this end of their edge - see collectEdgeTransitions.
        // Sequenced as a batch of their own, and emitted before the first instruction's own copies:
        // both stand at the block's entry point, and this one is what establishes the locations the
        // operand copies behind it read from.
        if(l.edges.any && l.edges.atEntry[block->index].isNotEmpty()) {
            auto& pending = l.scratch.edgeIn;
            auto& entry = l.scratch.blockEntry;
            pending.clear();
            entry.clear();

            for(auto& move: l.edges.atEntry[block->index]) pending.push(move);

            auto temps = l.moveTemps();
            sequenceMoves(temps, pending, entry, l.scratch.done);
            if(!l.measuring) blockRegs.entryMoves = commitSlice(l.records, entry);
        }

        for(auto i: block->instructions.contents(base)) {
            l.resolveInst(base[i], index);

            // The measuring pass reads nothing but the scratch registers it was made to ask for, so
            // it stops here rather than committing a record nothing will look at.
            if(!l.measuring) blockRegs.insts.push(l.commit());
            index++;
        }

        assertTrue(block->terminator != nullptr);
        l.resolveInst(base[block->terminator], index);

        // Phi copies run after whatever the terminator itself needs, and after the entry copies in
        // the entry block - a phi may be fed by an argument, which has to have reached its home
        // first. transformFunction guarantees that a block reaching any phi has a single successor,
        // so these copies cannot execute on a path that bypasses the phis.
        auto& pending = l.scratch.phiMoves;
        pending.clear();

        for(auto successor: block->outgoing) {
            if(!successor) continue;

            assertTrue(base[successor]->phis.isEmpty() || !(block->outgoing[0] && block->outgoing[1]));
            l.resolvePhis(block, base[successor], index, pending);
        }

        // The edge copies that go at *this* end of their edge, in the same batch as the phi
        // transfers: the two are one simultaneous set - see collectEdgeTransitions.
        if(l.edges.any) {
            for(auto& move: l.edges.atExit[block->index]) pending.push(move);
        }

        auto temps = l.moveTemps();
        auto& terminatorMoves = l.scratch.regs.moves;
        if(index == 0) sequenceMoves(temps, entryMoves, terminatorMoves, l.scratch.done);
        sequenceMoves(temps, pending, terminatorMoves, l.scratch.done);

        if(!l.measuring) blockRegs.insts.push(l.commit());
        index++;

        if(!l.measuring) result.blocks.add(block, ::move(blockRegs));
    }

    // §7.2.3, over the finished records: it reads two blocks at once, so it cannot be part of a walk
    // that has only reached one of them.
    if(!l.measuring) hoistCommonEntryMoves(l, base, fun, result);

    result.writtenPhysical = l.written;
}

void legalizeFunction(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement, const TemporaryReserve& temporaries,
    RegScratch& scratch, LegalizedFunction& out)
{
    if(!scratch.legalize) scratch.legalize = new LegalizeScratch();

    Legalizer l(base, fun, live, machine, constraints, placement, temporaries, scratch);
    runLegalizer(l, base, fun, out);
}

TemporaryReserve measureTemporaryReserve(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement, const TemporaryReserve& pool,
    RegScratch& scratch)
{
    if(!scratch.legalize) scratch.legalize = new LegalizeScratch();

    // Measured against the widest pools rather than against nothing, so that every temporary this
    // walk hands out is a register of its own: two of them naming one register would look like a copy
    // cycle the real pass does not have, and would be measured as a demand for a scratch register
    // nothing needs. The records this produces are discarded - only the counts are read.
    //
    // Over the *chosen* registers (§42), which is what `pool` is here for: `takeTemp` steps over a
    // position whose register this instruction's own expansion clobbers, so a measurement taken over
    // a different set of registers would step over a different set of clobbers.
    Legalizer l(base, fun, live, machine, constraints, placement, TemporaryReserve::widestLike(pool), scratch);
    l.measuring = true;

    // Nothing keeps what this produces, so it produces nothing: `measuring` stops the walk from
    // building an instruction record it is only going to throw away, and the scratch registers - the
    // one thing being measured - are handed out either way.
    LegalizedFunction discarded;
    runLegalizer(l, base, fun, discarded);
    return l.used;
}
