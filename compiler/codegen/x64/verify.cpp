#include "gen.h"
#include "x64_util.h"
#include "../../lower/lower_print.h"

/*
 * Allocation verifier.
 *
 * An allocation is wrong in only one way that matters: an instruction reads a location that does
 * not contain the value it was supposed to read. Everything else - two live values in one register,
 * a clobber that ate something still needed, a phi copy that never happened, two arms of a branch
 * disagreeing about where a value lives - is that same failure seen at the point where it finally
 * bites. So rather than re-deriving the allocator's own reasoning and checking it agrees with
 * itself, this walks the function keeping track of which value the emitted code will actually have
 * left in each register and stack slot, and compares that against what each instruction expects.
 *
 * The one thing it does take on trust is where a value is *supposed* to live: FunctionRegs carries
 * that as Allocation, and each block declares its own entry state from it. Every predecessor then
 * has to produce exactly that state at its terminator. Deriving a block's entry state from its
 * predecessors instead would make the check path-dependent in the same way the bug class is, and
 * two arms of a branch could quietly agree on being wrong.
 *
 * It knows nothing about how the allocation was arrived at - only FunctionRegs, the liveness sets
 * and the constraint tables - so it stays valid as the allocator gains live intervals with holes,
 * phi webs, stack homes and split locations. Allocation::locationOf is the seam: today it answers
 * from a single home per value, later from a segment list, and nothing here changes.
 *
 * Run from allocateRegisters in debug builds only (see the assertTrue there).
 *
 * Two things it does not model. An instruction's clobber mask is taken as a complete account of what
 * the encoder writes behind the operands' backs - an encoder that writes a register no mask mentions
 * is invisible here, exactly as it is to the allocator. And an X86Address's operand registers are
 * checked at the address instruction rather than at the load or store that folds it in, which is
 * sound only because the two are adjacent by construction.
 */

static const char* const kIntRegNames[kRegCount] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

static String locationName(RegId id) {
    if(id == kInvalidReg) return String("<nowhere>");

    auto index = U32(getRegIndex(id));

    switch(getRegClass(id)) {
        case GenReg:
            return index < kRegCount ? String(kIntRegNames[index]) : format(String("gen%@"), index);
        case XmmReg:
            return format(String("xmm%@"), index);
        case StackReg:
            return format(String("stack:%@"), index);
        default:
            return format(String("<bad location %@>"), U32(id));
    }
}

/*
 * What the machine holds where.
 *
 * One value id per location, which is all the model needs: a location holding two things at once is
 * exactly the state that cannot happen, so recording only the last thing written to it turns that
 * case into a failed lookup at the next read.
 */
struct MachineState {
    // Indexed by RegClass for the two classes that name physical registers. StackReg indices are
    // unbounded (a frame is as large as it needs to be), so those live in `slots`.
    LiveId regs[2][kRegCount];
    Array<LiveId> slots;

    MachineState() {
        for(auto& cls: regs) {
            for(auto& r: cls) r = kNullLive;
        }
    }

    static bool isRegister(RegId id) {
        auto cls = getRegClass(id);
        return (cls == GenReg || cls == XmmReg) && getRegIndex(id) < kRegCount;
    }

    LiveId get(RegId id) const {
        if(isRegister(id)) return regs[getRegClass(id)][getRegIndex(id)];
        if(getRegClass(id) != StackReg) return kNullLive;

        auto index = getRegIndex(id);
        return index < slots.size() ? slots[index] : kNullLive;
    }

    void set(RegId id, LiveId value) {
        if(isRegister(id)) {
            regs[getRegClass(id)][getRegIndex(id)] = value;
            return;
        }

        if(getRegClass(id) != StackReg) return;

        auto index = getRegIndex(id);
        while(slots.size() <= index) slots.push(kNullLive);
        slots[index] = value;
    }

    // Whatever was in these general registers is gone once the instruction has run. Nothing is
    // reported here: a clobbered value that was still needed shows up as a failed read at the
    // instruction that needed it, which is where the useful diagnostic is.
    void clobber(U64 mask) {
        for(Size i = 0; i < kRegCount; i++) {
            if(mask & (U64(1) << i)) regs[GenReg][i] = kNullLive;
        }
    }
};

struct Verifier {
    Context& ctx;
    LowerBase base;
    LowerFunction& fun;
    Liveness& live;
    const Constraints& constraints;
    const FunctionRegs& regs;

    String funName;
    bool ok = true;

    Verifier(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live, const Constraints& constraints, const FunctionRegs& regs):
        ctx(ctx), base(base), fun(fun), live(live), constraints(constraints), regs(regs), funName(ctx.findName(fun.name)) {}

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
        auto shape = shapeOf(base, constraints, fun, inst);
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
            auto at = instRegs.uses[i];

            // An implicit operand is folded into the instruction's encoding (an embedded immediate,
            // a compare consumed as flags, a direct call's target) and must not be given a location.
            if(isImplicit(v)) {
                if(at != kInvalidReg) {
                    fail("%@: %@: implicit operand %@ was given location %@",
                        funName, name, nameOf(v), locationName(at));
                }
                continue;
            }

            if(at == kInvalidReg) {
                fail("%@: %@: operand %@ has no location", funName, name, nameOf(v));
                continue;
            }

            auto want = wantForUse(base, inst, shape, i);
            if(want != kInvalidReg && at != want) {
                fail("%@: %@: operand %@ must be read from %@, but is read from %@",
                    funName, name, nameOf(v), locationName(want), locationName(at));
            }

            auto found = state.get(at);
            if(found != v->liveId()) {
                fail("%@: %@: operand %@ is read from %@, which holds %@",
                    funName, name, nameOf(v), locationName(at), nameOf(found));
            }
        }

        // Before the results, so that an instruction producing its result in a register it also
        // clobbers (rem's remainder in rdx) ends up holding the result rather than nothing.
        if(shape.c && !shape.isReturn) state.clobber(shape.c->clobber);

        for(Size i = 0; i < created.size(); i++) {
            auto& v = created[i];
            auto at = instRegs.creates[i];

            if(isImplicit(&v)) {
                if(at != kInvalidReg) {
                    fail("%@: %@: implicit result %@ was given location %@",
                        funName, name, nameOf(&v), locationName(at));
                }
                continue;
            }

            if(at == kInvalidReg) {
                fail("%@: %@: result %@ has no location", funName, name, nameOf(&v));
                continue;
            }

            auto want = wantForResult(inst, shape, i);
            if(want != kInvalidReg && at != want) {
                fail("%@: %@: result %@ must be produced in %@, but is produced in %@",
                    funName, name, nameOf(&v), locationName(want), locationName(at));
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
            auto at = regs.allocation.locationOf(id, set->firstIndex);

            if(at == kInvalidReg) {
                fail("%@: block %@: %@ is live on entry but has no location",
                    funName, nameOf(block), nameOf(id));
                return;
            }

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
        auto& call = constraints.getCall(fun.callType);
        U32 index[kRegClassCount] = {};

        for(auto offset: fun.args.contents(base)) {
            auto& result = base[offset]->result;
            if(isImplicit(&result)) continue;

            auto cls = classForType(result.type);
            auto classIndex = index[cls];
            auto incoming = classIndex < kMaxRegInputs ? call.constraints[cls].args[classIndex] : kInvalidReg;

            if(incoming == kInvalidReg) {
                // Out of argument registers: the value arrives on the stack, in the frame object
                // whose position among the incoming arguments matches this one's.
                incoming = incomingArgSlot(index[StackReg]);
                index[StackReg]++;

                if(incoming == kInvalidReg) {
                    fail("%@: argument %@ arrives on the stack but has no frame object",
                        funName, nameOf(&result));
                    continue;
                }
            } else {
                index[cls]++;
            }

            state.set(incoming, result.liveId());
        }
    }

    // The frame object holding incoming stack argument `ordinal`. Found by searching the frame
    // rather than by assuming slot ids are handed out in argument order, so that the check does not
    // quietly agree with the allocator about a numbering neither of them is entitled to assume.
    RegId incomingArgSlot(U32 ordinal) {
        auto& slots = regs.frame.slots;

        for(Size i = 0; i < slots.size(); i++) {
            if(slots[i].kind == StackSlotKind::IncomingArg && slots[i].ordinal == ordinal) {
                return makeRegId(StackReg, U16(i));
            }
        }

        return kInvalidReg;
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

            auto at = regs.allocation.locationOf(id, set->firstIndex);
            if(at == kInvalidReg) return; // already reported by buildEntryState

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

            auto at = regs.allocation.locationOf(result.liveId(), set->firstIndex);
            if(at == kInvalidReg) {
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
    for(Size i = 0; i < kRegCount; i++) {
        if(layout.savedRegs & (U64(1) << i)) savedCount++;
    }

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
                logError("%@: incoming argument %@ is at %@, inside this function's own frame",
                    funName, slot.ordinal, offset);
                ok = false;
            }
        } else if(offset < regionLow || offset + I32(slot.size) > regionHigh) {
            logError("%@: frame object %@ (%@ bytes at %@) falls outside the reserved region [%@, %@)",
                funName, U32(i), slot.size, offset, regionLow, regionHigh);
            ok = false;
        }

        // Nothing shares an address yet: a spill slot may be reused by values whose lives do not
        // overlap, but nothing creates one, so any overlap here is a layout error rather than
        // deliberate sharing.
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

bool verifyAllocation(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live, const Constraints& constraints, const FunctionRegs& regs) {
    Verifier v(ctx, base, fun, live, constraints, regs);
    auto entry = base[fun.blocks.get(base, 0)];

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        auto found = regs.blocks.get(block);
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
