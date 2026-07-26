#include "gen.h"
#include "x64_util.h"
#include "../../lower/lower_print.h"

/*
 * The checks on the allocation, one per boundary the pipeline has: what placement decided
 * (verifyPlacement), what legalization made of it (verifyAllocation), and what frame layout turned
 * the frame objects into (verifyFrameLayout). All three run inside assertTrue, so they compile away
 * entirely in a release build.
 *
 * Naming a location is what the first two both need, so it comes first.
 */

static const char* const kIntRegNames[kGprCount] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

static String locationName(MachineLocation at) {
    auto index = U32(at.index);

    switch(at.kind) {
        case LocationKind::Invalid:
            return String("<nowhere>");
        case LocationKind::Physical:
            if(at.bank == BankVector) return format(String("xmm%@"), index);
            return index < kGprCount ? String(kIntRegNames[index]) : format(String("gpr%@"), index);
        case LocationKind::StackSlot:
            return format(String("stack:%@"), index);
        case LocationKind::Rematerializable:
            return format(String("remat:%@"), index);
    }

    return format(String("<bad location %@>"), index);
}

/*
 * Placement verifier.
 *
 * The verifier below checks that the emitted code delivers every value to every instruction that
 * reads it, which is the failure that matters - but it checks it through the instructions, so a
 * placement mistake is reported at the read that finally bit rather than at the web that was placed
 * wrongly. This checks the placement on its own terms, before a single instruction has been
 * resolved against it, and so names the web.
 *
 * Four things have to hold, and each of them is a way for the allocation to be wrong that no golden
 * would show:
 *
 *   - every value that is live anywhere has somewhere to live;
 *   - no two values whose lives overlap share a location - which is over register *units*, since
 *     two views of one register are the same storage, and over slot ids, since a spill slot is only
 *     reused between webs that are never live at once;
 *   - each location is one a value of that type may actually occupy: the right bank, a register of
 *     its class, never the frame pointer in a function that has one, never a bank no encoder
 *     implements;
 *   - nothing is placed in a register something writes while it is live, which is the avoid-set
 *     rule seen from the result rather than from the computation that produced it.
 */

bool verifyPlacement(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live,
    const MachineFunction& machine, const Constraints& constraints, const Placement& placement, bool framePointer)
{
    auto funName = ctx.findName(fun.name);
    auto ok = true;

    auto name = [&](LiveId id) {
        auto v = live.getValue(id);
        return v->name ? ctx.findName(v->name) : format(String("value#%@"), U32(id));
    };

    auto fail = [&](auto&&... args) {
        ok = false;
        logError(args...);
    };

    // Values in the order their ids run, so that a disagreement is reported the same way twice.
    for(Size i = 0; i < placement.valueCount(); i++) {
        auto id = LiveId(i);
        auto v = live.getValue(id);
        auto interval = live.getInterval(id);
        auto at = placement.locationOf(id, interval.isEmpty() ? 0 : interval.first());

        if(interval.isEmpty() || isImplicit(v)) continue;

        if(!at.isValid()) {
            // A value nothing reads needs no location, and is not given one: an argument the
            // function ignores would otherwise hold a register across the entry copies, which is
            // exactly where a function with more arguments than it uses has the least to spare.
            if(v->uses.isEmpty()) continue;

            fail("%@: %@ is live and read but has no location", funName, name(id));
            continue;
        }

        if(at.isPhysical()) {
            auto reg = at.physicalReg();
            auto cls = classForType(v->type);

            if(reg.bank != targetRegisters().regClass(cls).bank) {
                fail("%@: %@ is placed in %@, which is not in the bank its type lives in",
                    funName, name(id), locationName(at));
            } else if(!targetRegisters().regClass(cls).allowedPhysical.has(reg)) {
                fail("%@: %@ is placed in %@, which its register class cannot name",
                    funName, name(id), locationName(at));
            } else if(!allocatableRegs().has(reg)) {
                fail("%@: %@ is placed in %@, which is never handed out",
                    funName, name(id), locationName(at));
            }

            // A value in rbp in a function that addresses its frame through rbp is silent memory
            // corruption rather than a visibly wrong register - see functionNeedsFramePointer.
            if(framePointer && reg == framePointerReg()) {
                fail("%@: %@ is placed in the frame pointer", funName, name(id));
            }

            // The register model describes banks whose moves, spills and encodings do not exist. A
            // location in one would be written out as an integer instruction with a vector register
            // number in it, which nothing downstream can notice.
            if(reg.bank != BankGpr) {
                fail("%@: %@ is placed in %@, which no encoder implements",
                    funName, name(id), locationName(at));
            }
        } else if(at.isStack()) {
            auto slot = at.stackSlot();

            if(slot >= placement.frame.slots.size()) {
                fail("%@: %@ is placed in %@, of which the frame has %@",
                    funName, name(id), locationName(at), U32(placement.frame.slots.size()));
            } else if(placement.frame.slots[slot].slotClass != stackSlotClassFor(v->type)) {
                // Slots are packed by width, so a value in one of the wrong width would be read and
                // written taking its neighbour with it.
                fail("%@: %@ is placed in %@, which is not the width of its type",
                    funName, name(id), locationName(at));
            }
        } else if(at.isRemat()) {
            if(at.rematId() >= placement.remats.size()) {
                fail("%@: %@ names recipe %@, of which there are %@",
                    funName, name(id), U32(at.rematId()), U32(placement.remats.size()));
            }
        }

        // Nothing else may be in the same place at the same time. Compared per value rather than per
        // web, so that two values wrongly merged into one web are caught here as well.
        for(Size j = 0; j < i; j++) {
            auto other = LiveId(j);
            auto otherAt = placement.locationOf(other, 0);
            if(!otherAt.isValid() || isImplicit(live.getValue(other))) continue;

            bool shares;
            if(at.isPhysical() && otherAt.isPhysical()) {
                // Over units rather than over names: writing eax destroys rax.
                shares = at.physicalReg().bank == otherAt.physicalReg().bank
                    && (unitsOf(at.physicalReg()) & unitsOf(otherAt.physicalReg())) != 0;
            } else {
                // A recipe is nowhere at all, so two values that share one are two definitions of
                // one location - which is exactly what a recipe cannot describe.
                shares = at == otherAt;
            }

            if(!shares) continue;
            if(!interval.overlaps(live.getInterval(other))) continue;

            fail("%@: %@ and %@ are both live and both placed in %@",
                funName, name(other), name(id), locationName(at));
        }
    }

    // A value that has to hold its location *across* an instruction cannot be in a register the
    // instruction writes behind its back. This is what computeAvoidSets arranges; checking it here
    // catches an avoid set that was built from a different shape than the one the instruction ends
    // up having.
    U32 index = 0;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        auto onInst = [&](LowerInst* inst) {
            auto shape = shapeOf(base, machine, constraints, fun, inst);
            auto mask = writtenRegisters(shape);

            if(!mask.isEmpty()) {
                for(Size i = 0; i < placement.valueCount(); i++) {
                    auto id = LiveId(i);
                    auto at = placement.locationOf(id, beforeInst(index));
                    if(!at.isPhysical() || !mask.has(at.physicalReg())) continue;
                    if(!live.getInterval(id).crosses(index)) continue;

                    fail("%@: %@: %@ is live across this and placed in %@, which it writes",
                        funName, nameForInst(base, *inst), name(id), locationName(at));
                }
            }

            index++;
        };

        for(auto i: block->instructions.contents(base)) onInst(base[i]);
        onInst(base[block->terminator]);
    }

    return ok;
}

/*
 * Selected-machine verifier.
 *
 * The first boundary, and the one every later check depends on: that each instruction of the
 * function was actually selected into a form, and that the form and the instruction agree about what
 * it has. validateMachineForms checks the table itself, once; this checks the table against the
 * program, per function, after the transform pipeline has run.
 *
 * The failures it catches are the ones a form table makes possible. A form describing more operands
 * than the instruction has would have placement reading constraints past the end of the operand
 * list; a form calling an operand an immediate when the peepholes did not embed one would have the
 * encoder writing bytes for a value that is still in a register; a form needing an extension the
 * target does not have would emit an instruction the machine faults on. None of these can be seen in
 * a golden file, because the compiler that produced them was internally consistent - just wrong.
 */

bool verifySelection(Context& ctx, LowerBase base, LowerFunction& fun, const MachineFunction& machine) {
    auto funName = ctx.findName(fun.name);
    auto ok = true;

    auto check = [&](LowerInst* inst) {
        auto selected = machine.insts.get(inst);
        auto name = nameForInst(base, *inst);

        auto fail = [&](StringView what) {
            ok = false;
            logError("%@: %@: %@", funName, name, what);
        };

        // Everything downstream asks the form what an instruction does, so an instruction selection
        // never reached is one the allocator would have to guess about.
        if(selected.isNothing()) {
            fail("was never given a machine form"_v);
            return;
        }

        auto& form = machineTarget().form(selected.unwrap().form);
        if(form.opcode != selected.unwrap().opcode) {
            fail("was given a form belonging to another opcode"_v);
            return;
        }

        if((form.requiredFeatures & ~targetFeatures()) != 0) {
            fail("was given a form needing a target feature this build does not have"_v);
        }

        // A call, a syscall and a return take their operands from a convention rather than from the
        // form, which states none of its own.
        if(form.conventionOperands) return;

        auto used = inst->used();
        auto created = inst->created();

        if(form.uses.size() > used.size() || form.defs.size() > created.size()) {
            fail("was given a form describing more operands than it has"_v);
            return;
        }

        for(Size i = 0; i < form.uses.size(); i++) {
            auto value = base[used[i]];

            switch(form.uses[i].kind) {
                case OperandConstraintKind::Immediate:
                    // The encoding carries this operand's value in its own bytes, so there has to be
                    // a constant there to carry.
                    if(value->inst()->kind != LowerInst::Imm) fail("carries an immediate that is not a constant"_v);
                    break;

                case OperandConstraintKind::Address:
                    // Either a folded addressing mode or a pointer the allocator will leave in a
                    // register; anything else has no address for the encoder to write.
                    if(!isMem(value) && !isPtr(value->type)) fail("addresses an operand that is not a pointer"_v);
                    break;

                case OperandConstraintKind::None:
                    // An operand the encoding swallowed occupies no location, which is what being
                    // implicit means - and what stops the allocator from giving it one.
                    if(!isImplicit(value)) fail("folds an operand that still needs a location"_v);
                    break;

                default:
                    break;
            }
        }
    };

    for(auto a: fun.args.contents(base)) check((LowerInst*)base[a]);

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        for(auto p: block->phis.contents(base)) check(base[p]);
        for(auto i: block->instructions.contents(base)) check(base[i]);
        check(base[block->terminator]);
    }

    return ok;
}

/*
 * Legalization verifier.
 *
 * A legalized function is wrong in only one way that matters: an instruction reads a location that
 * does not contain the value it was supposed to read. Everything else - two live values in one
 * register, a clobber that ate something still needed, a phi copy that never happened, two arms of a
 * branch disagreeing about where a value lives - is that same failure seen at the point where it
 * finally bites. So rather than re-deriving the allocator's own reasoning and checking it agrees
 * with itself, this walks the function keeping track of which value the emitted code will actually
 * have left in each register and stack slot, and compares that against what each instruction
 * expects.
 *
 * The one thing it does take on trust is where a value is *supposed* to live: FunctionRegs carries
 * that as its Placement, and each block declares its own entry state from it. Every predecessor then
 * has to produce exactly that state at its terminator. Deriving a block's entry state from its
 * predecessors instead would make the check path-dependent in the same way the bug class is, and
 * two arms of a branch could quietly agree on being wrong. Whether the placement it trusts is itself
 * consistent is verifyPlacement's question, above.
 *
 * It knows nothing about how the allocation was arrived at - only FunctionRegs, the liveness sets
 * and the selected machine forms - so it stays valid as the allocator gains live intervals with
 * holes, phi webs, stack homes and split locations. Placement::locationOf is the seam: today it
 * answers from one segment per web, later from several, and nothing here changes.
 *
 * Run from allocateRegisters in debug builds only (see the assertTrue there).
 *
 * Two things it does not model. An instruction's clobber mask is taken as a complete account of what
 * the encoder writes behind the operands' backs - an encoder that writes a register no mask mentions
 * is invisible here, exactly as it is to the allocator. And an X86Address's operand registers are
 * checked at the address instruction rather than at the load or store that folds it in, which is
 * sound only because the two are adjacent by construction.
 */

/*
 * What the machine holds where.
 *
 * One value id per location, which is all the model needs: a location holding two things at once is
 * exactly the state that cannot happen, so recording only the last thing written to it turns that
 * case into a failed lookup at the next read.
 */
struct MachineState {
    // Indexed by bank for the physical registers. Slot ids are unbounded (a frame is as large as it
    // needs to be), so those live in `slots`.
    LiveId regs[kRegisterBankCount][kMaxRegistersPerBank];
    Array<LiveId> slots;

    MachineState() {
        for(auto& bank: regs) {
            for(auto& r: bank) r = kNullLive;
        }
    }

    static bool isRegister(MachineLocation at) {
        return at.isPhysical() && at.index < kMaxRegistersPerBank;
    }

    LiveId get(MachineLocation at) const {
        if(isRegister(at)) return regs[at.bank][at.index];
        if(!at.isStack()) return kNullLive;

        return at.index < slots.size() ? slots[at.index] : kNullLive;
    }

    void set(MachineLocation at, LiveId value) {
        if(isRegister(at)) {
            regs[at.bank][at.index] = value;
            return;
        }

        if(!at.isStack()) return;

        while(slots.size() <= at.index) slots.push(kNullLive);
        slots[at.index] = value;
    }

    // Whatever was in these registers is gone once the instruction has run. Nothing is reported
    // here: a clobbered value that was still needed shows up as a failed read at the instruction
    // that needed it, which is where the useful diagnostic is.
    void clobber(const RegSet& mask) {
        mask.iterate([&](PhysicalReg reg) {
            if(reg.index < kMaxRegistersPerBank) regs[reg.bank][reg.index] = kNullLive;
        });
    }
};

struct Verifier {
    Context& ctx;
    LowerBase base;
    LowerFunction& fun;
    Liveness& live;
    const MachineFunction& machine;
    const Constraints& constraints;
    const FunctionRegs& regs;

    String funName;
    bool ok = true;

    // Which value each recipe recreates, recovered from the allocation rather than taken on trust:
    // a recipe describes one web and a web has one location, so the map back is a search for the
    // location. It is what lets a materialization be checked like any other copy - what the machine
    // ends up holding is the value whose recipe it was.
    Array<LiveId> rematOwner;

    Verifier(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
        const Constraints& constraints, const FunctionRegs& regs):
        ctx(ctx), base(base), fun(fun), live(live), machine(machine), constraints(constraints), regs(regs),
        funName(ctx.findName(fun.name))
    {
        for(Size i = 0; i < regs.placement.remats.size(); i++) rematOwner.push(kNullLive);

        for(Size i = 0; i < regs.placement.valueCount(); i++) {
            auto at = regs.placement.locationOf(LiveId(i), 0);
            if(!at.isRemat()) continue;

            auto index = at.rematId();
            if(index >= rematOwner.size()) {
                fail("%@: %@ names recipe %@, of which there are %@",
                    funName, nameOf(LiveId(i)), U32(index), U32(regs.placement.remats.size()));
                continue;
            }

            if(rematOwner[index] != kNullLive) {
                fail("%@: %@ and %@ share recipe %@",
                    funName, nameOf(rematOwner[index]), nameOf(LiveId(i)), U32(index));
                continue;
            }

            rematOwner[index] = LiveId(i);
        }
    }

    template<Size length, class... Args>
    void fail(const char (&fmt)[length], Args&&... args) {
        ok = false;
        logError(fmt, forward<Args>(args)...);
    }

    String nameOf(LiveId id) {
        if(id == kNullLive) return String("nothing");

        auto v = live.getValue(id);
        return v->name ? ctx.findName(v->name) : format(String("value#%@"), U32(id));
    }

    String nameOf(LowerValue* v) {
        return v->name ? ctx.findName(v->name) : format(String("value#%@"), U32(v->liveId()));
    }

    String nameOf(LowerBlock* b) {
        return b->name ? ctx.findName(b->name) : format(String("#%@"), I32(b->index));
    }

    // Runs a sequenced parallel copy. A step whose source holds nothing is reported here rather
    // than at the eventual read, because a copy out of an empty register means the allocator
    // believes a value is somewhere it is not, and the register it names is the useful detail.
    void applyMoves(MachineState& state, const Array<RegMove>& moves, LowerInst* inst) {
        for(auto& m: moves) {
            if(m.from == m.to) continue;

            // A recipe as the source reads nothing: the value is recreated, so the destination ends
            // up holding it whatever was there before.
            if(m.from.isRemat()) {
                auto index = m.from.rematId();
                auto owner = index < rematOwner.size() ? rematOwner[index] : kNullLive;

                if(owner == kNullLive) {
                    fail("%@: %@: materializes recipe %@, which belongs to no value",
                        funName, nameForInst(base, *inst), U32(index));
                }

                if(!MachineState::isRegister(m.to)) {
                    fail("%@: %@: recipe %@ is materialized into %@, which is not a register",
                        funName, nameForInst(base, *inst), U32(index), locationName(m.to));
                }

                state.set(m.to, owner);
                continue;
            }

            if(state.get(m.from) == kNullLive) {
                fail("%@: %@: copy from %@ to %@ reads a location that holds nothing",
                    funName, nameForInst(base, *inst), locationName(m.from), locationName(m.to));
            }

            if(m.swap) {
                auto from = state.get(m.from);
                auto to = state.get(m.to);
                state.set(m.from, to);
                state.set(m.to, from);
            } else {
                state.set(m.to, state.get(m.from));
            }
        }
    }

    void checkInst(MachineState& state, LowerInst* inst, const InstRegs& instRegs, U32 index) {
        auto shape = shapeOf(base, machine, constraints, fun, inst);
        auto used = inst->used();
        auto created = inst->created();
        auto name = nameForInst(base, *inst);

        if(instRegs.uses.size() != used.size() || instRegs.creates.size() != created.size()) {
            fail("%@: %@: allocated %@ operand and %@ result locations for an instruction with %@ and %@",
                funName, name, U32(instRegs.uses.size()), U32(instRegs.creates.size()),
                U32(used.size()), U32(created.size()));
            return;
        }

        applyMoves(state, instRegs.moves, inst);

        for(Size i = 0; i < used.size(); i++) {
            auto v = base[used[i]];
            auto at = instRegs.uses[i].at;

            // An implicit operand is folded into the instruction's encoding (an embedded immediate,
            // a compare consumed as flags, a direct call's target) and must not be given a location.
            if(isImplicit(v)) {
                if(at.isValid()) {
                    fail("%@: %@: implicit operand %@ was given location %@",
                        funName, name, nameOf(v), locationName(at));
                }
                continue;
            }

            if(!at.isValid()) {
                fail("%@: %@: operand %@ has no location", funName, name, nameOf(v));
                continue;
            }

            auto want = wantForUse(shape, i);
            if(want.isValid() && at != want) {
                fail("%@: %@: operand %@ must be read from %@, but is read from %@",
                    funName, name, nameOf(v), locationName(want), locationName(at));
            }

            // An operand left in the frame has to be one the instruction has a memory form for -
            // either a memory source, or the read-modify-write destination, which is the same
            // operand read and written through one r/m field and so has to be the result's location
            // as well. Otherwise it reaches an encoder with nothing but a slot to put in a ModRM
            // byte, which is a failed assertion in gen.cpp rather than a wrong register visible
            // anywhere here.
            auto inPlace = memoryDefOperand(base, machine, inst) == I32(i)
                && instRegs.creates.size() > 0 && instRegs.creates[0].at == at;

            if(at.isStack() && !inPlace && memoryUseOperand(base, machine, inst) != I32(i)) {
                fail("%@: %@: operand %@ is read from %@, which no form of this instruction can address",
                    funName, name, nameOf(v), locationName(at));
            }

            // Nothing holds a rematerialized value, so nothing can read one in place: the recipe is
            // materialized into a register by the copies in front of the instruction instead.
            if(at.isRemat()) {
                fail("%@: %@: operand %@ is read from %@, which holds nothing at any point",
                    funName, name, nameOf(v), locationName(at));
                continue;
            }

            auto found = state.get(at);
            if(found != v->liveId()) {
                fail("%@: %@: operand %@ is read from %@, which holds %@",
                    funName, name, nameOf(v), locationName(at), nameOf(found));
            }
        }

        // Before the results, so that an instruction producing its result in a register it also
        // clobbers (rem's remainder in rdx) ends up holding the result rather than nothing.
        if(!shape.isReturn) state.clobber(shape.clobber);

        for(Size i = 0; i < created.size(); i++) {
            auto& v = created[i];
            auto at = instRegs.creates[i].at;

            if(isImplicit(&v)) {
                if(at.isValid()) {
                    fail("%@: %@: implicit result %@ was given location %@",
                        funName, name, nameOf(&v), locationName(at));
                }
                continue;
            }

            if(!at.isValid()) {
                fail("%@: %@: result %@ has no location", funName, name, nameOf(&v));
                continue;
            }

            auto want = wantForResult(shape, i);
            if(want.isValid() && at != want) {
                fail("%@: %@: result %@ must be produced in %@, but is produced in %@",
                    funName, name, nameOf(&v), locationName(want), locationName(at));
            }

            // A result written straight into the frame is only legal in the one form that has a
            // memory destination, and only when the operand that form reads through the same r/m
            // field is in that very slot. Anywhere else the encoder has no address to write to.
            if(at.isStack()) {
                auto operand = memoryDefOperand(base, machine, inst);
                auto inPlace = i == 0 && operand != kNoMemoryOperand
                    && Size(operand) < instRegs.uses.size() && instRegs.uses[operand].at == at;

                if(!inPlace) {
                    fail("%@: %@: result %@ is produced in %@, which no form of this instruction can write",
                        funName, name, nameOf(&v), locationName(at));
                }
            }

            // A rematerialized result is produced nowhere at all - the instruction emits nothing,
            // and every reader recreates the value for itself. It has to be the value's own home:
            // a recipe is not a place something can be put.
            if(at.isRemat() && regs.placement.locationOf(v.liveId(), index) != at) {
                fail("%@: %@: result %@ is produced as %@, which is not its own recipe",
                    funName, name, nameOf(&v), locationName(at));
            }

            state.set(at, v.liveId());
        }

        applyMoves(state, instRegs.postMoves, inst);
    }

    // The state the allocation claims holds at a block's first instruction. Every value live into
    // the block sits in the location the allocation gives it there, and no two of them share one.
    void buildEntryState(MachineState& state, LowerBlock* block) {
        auto set = live.getBlock(block);

        set->liveIn.iterate(set->valueCount, [&](Size raw) {
            auto id = LiveId(raw);
            auto at = regs.placement.locationOf(id, set->firstIndex);

            if(!at.isValid()) {
                fail("%@: block %@: %@ is live on entry but has no location",
                    funName, nameOf(block), nameOf(id));
                return;
            }

            // A rematerialized value is available at every point it is live without occupying
            // anything, so it neither has to arrive here nor can collide with what does.
            if(at.isRemat()) return;

            auto existing = state.get(at);
            if(existing != kNullLive) {
                fail("%@: block %@: %@ and %@ are both live on entry and both allocated to %@",
                    funName, nameOf(block), nameOf(existing), nameOf(id), locationName(at));
                return;
            }

            state.set(at, id);
        });
    }

    // The entry block's state is not declared by the allocation but by the calling convention:
    // arguments are already in the registers the caller left them in, and the copies that move them
    // to their homes are the first thing the function runs. Re-deriving the placement from the
    // convention tables (rather than reading it back off the allocation) is the point - it is the
    // one place where what the machine holds is not something the allocator got to choose.
    void buildArgState(MachineState& state) {
        auto args = fun.args.contents(base);

        Array<ArgLocation> locations;
        classifyArgs(constraints.getConvention(fun.callType), args.size(), [&](Size i) {
            return base[args[i]]->result.type;
        }, locations);

        for(Size i = 0; i < args.size(); i++) {
            auto& result = base[args[i]]->result;
            if(isImplicit(&result)) continue;

            auto incoming = MachineLocation::physical(locations[i].reg);

            if(locations[i].kind == ArgLocation::Stack) {
                incoming = incomingArgSlot(locations[i].stackOffset);

                if(!incoming.isValid()) {
                    fail("%@: argument %@ arrives on the stack but has no frame object",
                        funName, nameOf(&result));
                    continue;
                }
            }

            state.set(incoming, result.liveId());
        }
    }

    // The frame object holding the incoming stack argument at `offset`. Found by searching the frame
    // rather than by assuming slot ids are handed out in argument order, so that the check does not
    // quietly agree with the allocator about a numbering neither of them is entitled to assume.
    MachineLocation incomingArgSlot(U32 offset) {
        auto& slots = regs.placement.frame.slots;

        for(Size i = 0; i < slots.size(); i++) {
            if(slots[i].kind == StackSlotKind::IncomingArg && slots[i].argOffset == offset) {
                return MachineLocation::stack(StackSlotId(i));
            }
        }

        return MachineLocation::invalid();
    }

    // Whether what `from` leaves behind is what `to` declared it would find. This is the check that
    // rules out path-dependent locations: it runs on every edge, so an arm of a branch that put a
    // value somewhere else has nowhere to hide.
    void checkEdge(MachineState& state, LowerBlock* from, LowerBlock* to) {
        auto set = live.getBlock(to);

        set->liveIn.iterate(set->valueCount, [&](Size raw) {
            auto id = LiveId(raw);
            auto v = live.getValue(id);

            // A phi is live into its own block but is not carried there by anything: what has to
            // arrive in its location is the value this particular edge feeds it, checked below.
            auto inst = v->inst();
            if(isPhi(inst) && base[inst->block] == to) return;

            auto at = regs.placement.locationOf(id, set->firstIndex);
            if(!at.isValid()) return; // already reported by buildEntryState
            if(at.isRemat()) return;  // carried by nothing, so nothing has to carry it here

            auto found = state.get(at);
            if(found != id) {
                fail("%@: on the edge from block %@ to block %@: %@ is live and allocated to %@, which holds %@",
                    funName, nameOf(from), nameOf(to), nameOf(id), locationName(at), nameOf(found));
            }
        });

        for(auto p: to->phis.contents(base)) {
            auto phi = base[p];
            auto& result = phi->result;
            if(isImplicit(&result)) continue;

            auto sources = phi->sources();
            auto incoming = phi->used();
            LowerValue* value = nullptr;

            for(Size i = 0; i < sources.size(); i++) {
                if(base[sources[i]] == from) { value = base[incoming[i]]; break; }
            }

            // Not an edge this phi takes a value from.
            if(!value || isImplicit(value)) continue;

            auto at = regs.placement.locationOf(result.liveId(), set->firstIndex);
            if(!at.isValid()) {
                fail("%@: phi %@ in block %@ has no location", funName, nameOf(&result), nameOf(to));
                continue;
            }

            auto found = state.get(at);
            if(found != value->liveId()) {
                fail("%@: on the edge from block %@ to block %@: phi %@ takes %@ from here and lives in %@, which holds %@",
                    funName, nameOf(from), nameOf(to), nameOf(&result), nameOf(value), locationName(at), nameOf(found));
            }
        }
    }
};

/*
 * Frame layout verifier.
 *
 * The allocation verifier below checks *which* location holds a value, and would be equally happy
 * with two frame objects that share an address. This checks the other half: that the offsets frame
 * layout produced describe a frame the objects actually fit in, and that no two of them land on top
 * of each other. Both failures corrupt memory rather than producing an obviously wrong register, so
 * neither shows up in a golden.
 */
bool verifyFrameLayout(Context& ctx, LowerFunction& fun, const FrameObjects& objects, const FrameLayout& layout) {
    auto funName = ctx.findName(fun.name);
    auto ok = true;

    if(layout.slotOffset.size() != objects.slots.size()) {
        logError("%@: frame layout placed %@ of %@ frame objects",
            funName, U32(layout.slotOffset.size()), U32(objects.slots.size()));
        return false;
    }

    // A dynamic alloca moves rsp, so everything fixed has to be reachable from somewhere else.
    if(objects.hasDynamicAlloca && !layout.framePointer) {
        logError("%@: the frame moves at run time but has no frame pointer", funName);
        ok = false;
    }

    U32 savedCount = 0;
    layout.savedRegs.iterate([&](PhysicalReg) { savedCount++; });

    // Where the region the prologue reserved sits relative to the base register. Below the saved
    // registers when the base is rbp, and directly at the stack pointer when it is not.
    auto regionLow = layout.framePointer ? -I32(8 * savedCount + layout.fixedSize) : 0;
    auto regionHigh = layout.framePointer ? -I32(8 * savedCount) : I32(layout.fixedSize);

    for(Size i = 0; i < objects.slots.size(); i++) {
        auto& slot = objects.slots[i];
        auto offset = layout.slotOffset[i];

        if(offset % I32(slot.alignment) != 0) {
            logError("%@: frame object %@ needs %@-byte alignment but sits at %@",
                funName, U32(i), slot.alignment, offset);
            ok = false;
        }

        if(slot.kind == StackSlotKind::IncomingArg) {
            // In the caller's frame, above the return address - never in the region this function
            // reserved for itself.
            if(offset < 8) {
                logError("%@: incoming argument at %@ resolves to %@, inside this function's own frame",
                    funName, slot.argOffset, offset);
                ok = false;
            }
        } else if(offset < regionLow || offset + I32(slot.size) > regionHigh) {
            logError("%@: frame object %@ (%@ bytes at %@) falls outside the reserved region [%@, %@)",
                funName, U32(i), slot.size, offset, regionLow, regionHigh);
            ok = false;
        }

        // Two frame objects may never share bytes. Reuse of a spill slot between webs whose lives
        // do not overlap happens a level up - they share one slot id, and so one address - so
        // anything that overlaps here is a layout error rather than deliberate sharing.
        for(Size j = 0; j < i; j++) {
            auto& other = objects.slots[j];
            auto otherOffset = layout.slotOffset[j];

            if(offset < otherOffset + I32(other.size) && otherOffset < offset + I32(slot.size)) {
                logError("%@: frame objects %@ and %@ overlap at %@ and %@",
                    funName, U32(j), U32(i), otherOffset, offset);
                ok = false;
            }
        }
    }

    return ok;
}

bool verifyAllocation(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine, const Constraints& constraints, const FunctionRegs& regs) {
    Verifier v(ctx, base, fun, live, machine, constraints, regs);
    auto entry = base[fun.blocks.get(base, 0)];

    // rbp is either the frame pointer or a register values live in, and which one is decided before
    // allocation runs. A value found in it in a function that establishes a frame pointer means the
    // two halves of that decision disagreed, which corrupts the frame rather than producing visibly
    // wrong code, so it is checked here rather than left to show up in the emitted bytes.
    if(regs.framePointer) {
        for(Size i = 0; i < regs.placement.valueCount(); i++) {
            if(regs.placement.locationOf(LiveId(i), 0) == MachineLocation::physical(framePointerReg())) {
                v.fail("%@: %@ is allocated to the frame pointer", v.funName, v.nameOf(LiveId(i)));
            }
        }
    }

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        auto found = regs.legalized.blocks.get(block);
        if(!found.isJust()) {
            v.fail("%@: block %@ has no allocation", v.funName, v.nameOf(block));
            continue;
        }

        auto& blockRegs = found.unwrap();
        auto insts = block->instructions.contents(base);

        if(blockRegs.insts.size() != insts.size() + 1) {
            v.fail("%@: block %@ has %@ allocated instructions for %@ instructions and a terminator",
                v.funName, v.nameOf(block), U32(blockRegs.insts.size()), U32(insts.size()));
            continue;
        }

        MachineState state;
        if(block == entry) {
            v.buildArgState(state);
        } else {
            v.buildEntryState(state, block);
        }

        auto index = live.getBlock(block)->firstIndex;

        for(Size i = 0; i < insts.size(); i++) {
            v.checkInst(state, base[insts[i]], blockRegs.insts[i], index);
            index++;
        }

        v.checkInst(state, base[block->terminator], blockRegs.insts[insts.size()], index);

        for(auto successor: block->outgoing) {
            if(!successor) continue;
            v.checkEdge(state, block, base[successor]);
        }
    }

    return v.ok;
}
