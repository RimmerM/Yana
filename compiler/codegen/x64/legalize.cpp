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
static bool writesSource(const Array<RegMove>& pending, const Array<bool>& done, Size i) {
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
static void sequenceMoves(MoveTemps& temps, Array<RegMove>& pending, Array<RegMove>& out) {
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

            MachineLocation reads;

            auto exchangeable = move.from.isPhysical() && move.to.isPhysical()
                && classHasExchange(move.regClass);

            if(exchangeable) {
                out.push(RegMove { move.from, move.to, move.regClass, true });
                reads = move.from;
            } else {
                // No exchange to reach for - a slot at one end, or a class the machine has no
                // exchange instruction for: park the destination somewhere first.
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
        auto scratch = temps.take(regClass, 1);
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
    for(auto& web: placement.webs) {
        for(Size i = 1; i < web.segments.size(); i++) {
            auto& previous = web.segments[i - 1];
            auto& segment = web.segments[i];
            if(segment.location == previous.location) continue;

            // Nothing carries a value across a hole, so two segments with a hole between them are
            // two stretches of one location and never a boundary. Placement builds them that way;
            // reaching here with a gap would mean a value expected somewhere nothing put it.
            assertTrue(segment.from == previous.to);

            out.push(SegmentTransition {
                .index = (segment.from - 1) / 2,
                .post = (segment.from & 1) == 0,
                .move = RegMove { previous.location, segment.location, web.regClass },
            });
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
    if(i == 0 && destructiveReg.isValid()) return UseSite { destructiveReg };

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
static MachineAddress computedAddress(LowerInstX86Address& addr, const Array<ResolvedOperand>& uses) {
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

struct Legalizer {
    LowerBase base;
    LowerFunction& fun;
    const MachineFunction& machine;
    const Constraints& constraints;
    const Placement& placement;

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
    // in front of its user and resolved just before it.
    HashMap<LowerInst*, MachineAddress> addresses;

    // Scratch registers handed out within the instruction currently being resolved, reset for each
    // one. A value whose home is a frame slot cannot be read by an encoder, so it is brought into
    // one of these first - and taken back to the frame afterwards if the instruction wrote it.
    Size tempsUsed[kRegisterBankCount] = {};

    // The copies that cross a split web's segment boundaries, in instruction order, and how far the
    // walk has read into them. One cursor serves the function because the walk visits every
    // instruction index once and in order.
    Array<SegmentTransition> transitions;
    Size transitionCursor = 0;

    Legalizer(LowerBase base, LowerFunction& fun, const MachineFunction& machine,
        const Constraints& constraints, const Placement& placement, const TemporaryReserve& reserve):
        base(base), fun(fun), machine(machine), constraints(constraints), placement(placement),
        reserve(reserve)
    {
        collectTransitions(placement, transitions);
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

        assertTrue(index < kMaxOperandTemps);                          // more than any form can want
        if(!measuring) assertTrue(index < reserve.operandTemps[bank]);  // ... than the reserve holds back
        if(index + 1 > used.operandTemps[bank]) used.operandTemps[bank] = U8(index + 1);

        auto reg = reserve.operandTemp(bank, index);
        written.add(reg);
        return MachineLocation::physical(reg);
    }

    // Where a value lives at the instruction numbered `index`, which by this point is settled:
    // placement ran to completion before any of this did. One query serves an instruction's operands
    // and its results alike, since a web occupies one location for the whole of an instruction - a
    // split that ended a segment in the middle of one would have nowhere legal to put the transfer.
    MachineLocation homeOf(LowerValue* v, U32 index) {
        auto home = placement.locationOf(v, beforeInst(index));
        assertTrue(home.isValid()); // a value placement never reached
        return home;
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

        auto at = direct.at;
        assertTrue(at.isPhysical() && at.bank == BankGpr); // a pointer operand that is not a register
        return MachineAddress::atRegister(U8(at.index));
    }

    // The one memory address this instruction references, if its encoding has an address field at
    // all - see the block comment above.
    void resolveAddress(LowerInst* inst, InstRegs& out) {
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
                addresses.add(inst, computedAddress(*(LowerInstX86Address*)inst, out.uses));
                break;

            case LowerInst::X86Lea:
                set(computedAddress(*(LowerInstX86Address*)inst, out.uses));
                break;

            case LowerInst::X86PushArg:
                // The outgoing argument area is always addressed through rsp: it is the lowest part
                // of the frame and reserved once by the prologue, so it stays where the callee looks
                // for it whatever else the function does to its stack.
                set(MachineAddress::atOffset(U8(IntRegister::rsp), I32(((LowerInstX86PushArg*)inst)->stackOffset)));
                break;

            case LowerInst::Global:
                set(MachineAddress::atSymbol(nullptr, base[((LowerInstGlobal*)inst)->target]));
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

    InstRegs resolveInst(LowerInst* inst, U32 index) {
        InstRegs out;

        // The two parallel copies this instruction needs: the transfers that put its operands where
        // it reads them, and the ones that carry its results from where it writes them to their
        // homes. Both are *simultaneous* sets rather than sequences - an instruction with two
        // results in fixed registers can perfectly well have the first one's home be the second
        // one's register - so both are sequenced before they are emitted.
        Array<RegMove> pending;
        Array<RegMove> pendingPost;

        for(auto& used: tempsUsed) used = 0;

        auto shape = shapeOf(base, machine, constraints, fun, inst);
        auto used = inst->used();
        auto created = inst->created();

        // The destructive destination has to be resolved before anything else: it is where used()[0]
        // must sit by the time the instruction runs, so it is reported for both that operand and
        // the result. Placement already kept it off the registers the *other* operands are read
        // from, which is what makes the copy that puts used()[0] there safe to emit in front of the
        // instruction.
        MachineLocation destructiveReg;
        bool memoryDest = false;

        // The form states which operand the result is written over, if any. Every one described so
        // far ties to operand zero, which is what the code below assumes when it copies that operand
        // into the result's register; a form tying to any other would need that copy to move.
        auto tied = machine.formOf(inst).tiedResult();
        assertTrue(tied <= 0); // a result tied to an operand other than the first

        if(tied == 0 && used.size() > 0 && created.size() > 0 && !isImplicit(&created[0])) {
            destructiveReg = homeOf(&created[0], index);

            if(destructiveReg.isStack()) {
                // The result lives in the frame. Where the encoding has a form that writes its
                // destination through the r/m field and the operand it overwrites already occupies
                // that very slot, the whole operation happens in place - `add [rsp+8], rcx` - and
                // neither the reload nor the store exists. This is what a coalesced loop-carried
                // accumulator looks like once it has been spilled.
                auto choice = directMemoryOperands(base, machine, inst);
                auto operandHome = choice.hasReadWrite()
                    ? homeOf(base[used[choice.readWrite]], index)
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
            if(location.isValid() && location != homeOf(v, index)) {
                pending.push(RegMove { homeOf(v, index), location, regClass });
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
            auto home = homeOf(&v, index);

            // Where the encoder has to write it, which is the home unless the home is a frame slot
            // this instruction has no destination form for, or the encoding forces a particular
            // register. A recipe stays a recipe: nothing is written anywhere, and the instruction
            // that would have defined the value emits nothing at all.
            auto at = home;
            if(want.isValid()) at = want;
            else if(home.isStack()) at = takeTemp(bankForType(v.type));

            out.creates.push(ResolvedOperand::location(at, regClass));

            // A result produced somewhere other than its home is carried there afterwards. For a
            // fixed register nothing live can be sitting in the way: it is part of this
            // instruction's written set, which every web crossing the instruction avoids.
            if(at != home) pendingPost.push(RegMove { at, home, regClass });
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
        sequenceMoves(temps, pending, out.moves);
        sequenceMoves(temps, pendingPost, out.postMoves);
        return out;
    }

    // The copies carrying this block's outgoing values into a successor's phi locations. A phi that
    // shares a web with the value arriving over this edge is already where it needs to be, and the
    // transfer is an identity that sequenceMoves drops.
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

            auto from = homeOf(value, index);
            auto to = homeOf(&result, index);
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

// The one walk, run either to produce the instruction records or to measure what producing them
// costs in scratch registers - see measureTemporaryReserve. The two are the same pass because a
// separate rule for the demand would be a second answer to one question, and the one that is wrong
// is the one that leaves an instruction with nowhere to bring a spilled operand.
static LegalizedFunction runLegalizer(Legalizer& l, LowerBase base, LowerFunction& fun) {
    LegalizedFunction result;

    // The entry copies are emitted at index 0 below, which is only the first thing the function
    // executes because the implicit entry block holds no instructions - LowerFunction's constructor
    // creates it empty and nothing may branch to it, so its terminator is index 0. An entry block
    // with instructions would need them placed ahead of that instruction's own operand copies
    // instead.
    assertTrue(base[fun.blocks.get(base, 0)]->instructions.isEmpty());

    Array<RegMove> entryMoves;
    l.resolveArgs(entryMoves);

    U32 index = 0;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        BlockRegs blockRegs;

        for(auto i: block->instructions.contents(base)) {
            blockRegs.insts.push(l.resolveInst(base[i], index));
            index++;
        }

        assertTrue(block->terminator != nullptr);
        auto terminatorRegs = l.resolveInst(base[block->terminator], index);

        // Phi copies run after whatever the terminator itself needs, and after the entry copies in
        // the entry block - a phi may be fed by an argument, which has to have reached its home
        // first. transformFunction guarantees that a block reaching any phi has a single successor,
        // so these copies cannot execute on a path that bypasses the phis.
        Array<RegMove> pending;
        for(auto successor: block->outgoing) {
            if(!successor) continue;

            assertTrue(base[successor]->phis.isEmpty() || !(block->outgoing[0] && block->outgoing[1]));
            l.resolvePhis(block, base[successor], index, pending);
        }

        auto temps = l.moveTemps();
        if(index == 0) sequenceMoves(temps, entryMoves, terminatorRegs.moves);
        sequenceMoves(temps, pending, terminatorRegs.moves);

        blockRegs.insts.push(::move(terminatorRegs));
        index++;

        result.blocks.add(block, ::move(blockRegs));
    }

    result.writtenPhysical = l.written;
    return result;
}

LegalizedFunction legalizeFunction(LowerBase base, LowerFunction& fun, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement, const TemporaryReserve& temporaries)
{
    Legalizer l(base, fun, machine, constraints, placement, temporaries);
    return runLegalizer(l, base, fun);
}

TemporaryReserve measureTemporaryReserve(LowerBase base, LowerFunction& fun, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement)
{
    // Measured against the widest pools rather than against nothing, so that every temporary this
    // walk hands out is a register of its own: two of them naming one register would look like a copy
    // cycle the real pass does not have, and would be measured as a demand for a scratch register
    // nothing needs. The records this produces are discarded - only the counts are read.
    Legalizer l(base, fun, machine, constraints, placement, TemporaryReserve::widest());
    l.measuring = true;

    runLegalizer(l, base, fun);
    return l.used;
}
