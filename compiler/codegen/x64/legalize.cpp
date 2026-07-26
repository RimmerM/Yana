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
 * kMaxSpillTemps and allocateRegisters.
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

            MachineLocation reads;

            if(move.from.isStack() || move.to.isStack() || move.from.isRemat()) {
                // No exchange to reach for: park the destination somewhere first.
                auto scratch = MachineLocation::physical(moveTemp(move.to.isPhysical() ? move.to.bank : BankGpr, 0));
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
        if(out[i].from.isPhysical() || !out[i].to.isStack()) continue;

        auto scratch = MachineLocation::physical(moveTemp(BankGpr, 1));
        auto to = out[i].to;

        out[i].to = scratch;
        out.insert(i + 1, RegMove { scratch, to });
        i++;
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
    if(home.isStack() && !memoryDest && memoryUseOperand(base, machine, inst) == I32(i)) return UseSite { home };

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

    if(addr.base) {
        out.hasBase = true;
        out.base = physical(operand++);
    }

    if(addr.index) {
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

    Legalizer(LowerBase base, LowerFunction& fun, const MachineFunction& machine,
        const Constraints& constraints, const Placement& placement):
        base(base), fun(fun), machine(machine), constraints(constraints), placement(placement) {}

    MachineLocation takeTemp(RegisterBankId bank) {
        auto index = tempsUsed[bank]++;
        assertTrue(index < kMaxSpillTemps); // an instruction wanting more scratch than is reserved

        auto reg = spillTemp(bank, index);
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
    // `reserve` is false when the caller only wants to know where a sibling operand will be read
    // from, so that asking twice does not consume two scratch registers for one operand.
    MachineLocation useLocation(LowerInst* inst, const InstShape& shape, Size i, U32 index,
        MachineLocation destructiveReg, bool memoryDest, bool reserve)
    {
        auto site = useSiteOf(base, machine, placement, inst, shape, i, index, destructiveReg, memoryDest);
        if(!site.needsTemp) return site.at;

        return reserve
            ? takeTemp(site.tempBank)
            : MachineLocation::physical(spillTemp(site.tempBank, tempsUsed[site.tempBank]));
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

        switch(inst->kind) {
            case LowerInst::X86Address:
                // Emits nothing itself: it is resolved so that whichever access folds it in can name
                // the answer rather than working it out again.
                addresses.add(inst, computedAddress(*(LowerInstX86Address*)inst, out.uses));
                break;

            case LowerInst::X86Lea:
                set(computedAddress(*(LowerInstX86Address*)inst, out.uses));
                break;

            case LowerInst::Load:
                set(operandAddress(base[((LowerInstLoad*)inst)->from], out.uses[0]));
                break;

            case LowerInst::Store:
                set(operandAddress(base[((LowerInstStore*)inst)->to], out.uses[0]));
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
                auto memoryOperand = memoryDefOperand(base, machine, inst);
                auto first = base[used[0]];

                memoryDest = memoryOperand == 0 && !isImplicit(first) && homeOf(first, index) == destructiveReg;

                // Otherwise it is computed in a scratch register and stored afterwards, and the
                // operand it overwrites has to be brought into that same one.
                if(!memoryDest) {
                    auto slot = destructiveReg;
                    destructiveReg = takeTemp(bankForType(created[0].type));
                    pendingPost.push(RegMove { destructiveReg, slot });
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

            out.uses.push(ResolvedOperand::location(location));
            if(location.isValid() && location != homeOf(v, index)) {
                pending.push(RegMove { homeOf(v, index), location });
            }
        }

        for(Size i = 0; i < created.size(); i++) {
            auto& v = created[i];

            if(isImplicit(&v)) {
                out.creates.push(ResolvedOperand::none());
                continue;
            }

            if(i == 0 && destructiveReg.isValid()) {
                out.creates.push(ResolvedOperand::location(destructiveReg));
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

            out.creates.push(ResolvedOperand::location(at));

            // A result produced somewhere other than its home is carried there afterwards. For a
            // fixed register nothing live can be sitting in the way: it is part of this
            // instruction's written set, which every web crossing the instruction avoids.
            if(at != home) pendingPost.push(RegMove { at, home });
        }

        // A constant materialization carries the value it defines rather than an operand of its
        // own, which is what the form's immediate field naming a result says.
        auto& immField = machine.formOf(inst).encoding.immField;
        if(!immField.isNone() && immField.result) {
            assertTrue(inst->kind == LowerInst::Imm); // a form defining a constant that is not one
            out.creates[immField.index].immediate = ((LowerImm*)inst)->i;
            out.creates[immField.index].isImmediate = true;
        }

        resolveAddress(inst, out);
        sequenceMoves(pending, out.moves);
        sequenceMoves(pendingPost, out.postMoves);
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
            if(from != to) pending.push(RegMove { from, to });
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
            if(home != incoming) entryMoves.push(RegMove { incoming, home });
        }
    }
};

LegalizedFunction legalizeFunction(LowerBase base, LowerFunction& fun, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement)
{
    Legalizer l(base, fun, machine, constraints, placement);
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

        if(index == 0) sequenceMoves(entryMoves, terminatorRegs.moves);
        sequenceMoves(pending, terminatorRegs.moves);

        blockRegs.insts.push(::move(terminatorRegs));
        index++;

        result.blocks.add(block, ::move(blockRegs));
    }

    result.writtenPhysical = l.written;
    return result;
}
