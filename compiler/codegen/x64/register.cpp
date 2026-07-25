#include "gen.h"
#include "x64_util.h"

/*
 * Register allocation.
 *
 * Every value is given one register for the whole of its live range, and keeps it. Nothing is ever
 * relocated mid-function, so a value is in the same place on every path that reaches a given
 * instruction - which is what makes the result independent of how the blocks happen to be laid out.
 *
 * The two inputs that make that possible both come from lower_analyze.cpp: a linear numbering of
 * the instructions (in the order LowerFunction::blocks lists the blocks, which transformFunction
 * has put in reverse postorder), and a LiveRange per value in that numbering. Two values may share
 * a register exactly when their ranges do not overlap, and a value's range ends at its last read,
 * so a result may take over the register of an operand that dies at the same instruction.
 *
 * That leaves three things registers can't just be handed out for, all of which are handled by
 * *copying* around the fixed register rather than by moving the value's home:
 *
 *   - Fixed-register constraints (a divisor in rax, a call argument in rdi, ...). Operands are
 *     copied into place before the instruction and results copied out of place after it, so the
 *     value's home is unaffected. The copies are emitted as one parallel copy per instruction.
 *   - Clobbers. A value whose range *crosses* a clobbering instruction is simply never given one of
 *     the clobbered registers (see ClobberSite below), so there is nothing to rescue at the call.
 *   - Destructive two-address encodings, where the result overwrites its first operand's register.
 *     The result is allocated first, preferring that operand's register, and the operand is copied
 *     into the result's register when they differ.
 *
 * Phis are ordinary values here. Their register is decided at the first predecessor edge that
 * reaches them, and each predecessor ends with a parallel copy placing the incoming values into
 * the phi registers. transformFunction guarantees a block that needs such a copy has exactly one
 * successor, so the copy cannot run on a path that skips the phis.
 *
 * Not implemented: spilling. Running out of registers asserts rather than falling back to memory,
 * and splitting a live range is the natural place to start (see the README).
 */

inline RegClass classForType(LowerType type) {
    if(isIntLike(type)) {
        return GenReg;
    } else {
        return XmmReg;
    }
}

static constexpr Size kRegCount = 16;

// rsp and rbp are never handed out: rsp is required for push/pop/call/ret to work at all, and rbp
// is conventionally reserved for a frame pointer even though this MVP generates no prologue that
// sets one up yet.
static constexpr U64 kReservedRegs =
    (U64(1) << (Size)IntRegister::rsp) | (U64(1) << (Size)IntRegister::rbp);

inline U64 regBit(RegId reg) {
    return getRegClass(reg) == GenReg ? U64(1) << getRegIndex(reg) : 0;
}

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
 * Instruction constraint queries.
 *
 * The tables in constraint.cpp are indexed per register class and skip operands that don't occupy
 * a register at all, so the mapping from "operand N of this instruction" to "entry N of the table"
 * is not the identity. Resolving it here, from the instruction alone, keeps every caller from
 * having to thread a running per-class counter through its own loop.
 */

struct InstShape {
    const InstConstraints* c = nullptr;

    // First used() index that is a constrained argument. Call's used()[0] is the callee, not an
    // argument - except for a syscall, which has no callee at all: its used()[0] is the syscall
    // number, and is the first constrained operand.
    Size argStart = 0;

    // Set for a return, whose values are constrained like a call's *results* rather than its
    // arguments. A return's clobber set is deliberately ignored throughout: nothing is live once
    // the function has returned, so there is nothing left for it to protect.
    bool isReturn = false;
};

static InstShape shapeOf(LowerBase base, const Constraints& constraints, LowerFunction& fun, LowerInst* inst) {
    InstShape shape;

    if(inst->kind == LowerInst::Ret) {
        shape.c = &constraints.getCall(fun.callType);
        shape.isReturn = true;
        return shape;
    }

    shape.c = constraints.getConstraints(base, inst);

    if(inst->kind == LowerInst::Call && ((LowerInstCall*)inst)->getCallType() != LowerCallType::Syscall) {
        shape.argStart = 1;
    }

    return shape;
}

// The fixed register operand `i` has to be in when the instruction executes, if any.
static RegId wantForUse(LowerBase base, LowerInst* inst, const InstShape& shape, Size i) {
    if(!shape.c || i < shape.argStart) return kInvalidReg;

    auto used = inst->used();
    auto value = base[used[i]];
    if(isImplicit(value)) return kInvalidReg;

    auto cls = classForType(value->type);
    Size classIndex = 0;

    for(Size j = shape.argStart; j < i; j++) {
        auto other = base[used[j]];
        if(isImplicit(other)) continue;
        if(classForType(other->type) == cls) classIndex++;
    }

    if(classIndex >= kMaxRegInputs) return kInvalidReg;

    auto& table = shape.c->constraints[cls];
    return shape.isReturn ? table.results[classIndex] : table.args[classIndex];
}

// The fixed register result `i` is produced in, if any.
static RegId wantForResult(LowerInst* inst, const InstShape& shape, Size i) {
    if(!shape.c || shape.isReturn) return kInvalidReg;

    auto created = inst->created();
    if(isImplicit(&created[i])) return kInvalidReg;

    auto cls = classForType(created[i].type);
    Size classIndex = 0;

    for(Size j = 0; j < i; j++) {
        if(isImplicit(&created[j])) continue;
        if(classForType(created[j].type) == cls) classIndex++;
    }

    if(classIndex >= kMaxRegInputs) return kInvalidReg;
    return shape.c->constraints[cls].results[classIndex];
}

// Every general register this instruction writes behind the operands' backs: the ones it clobbers,
// plus the ones the parallel copy in front of it writes to satisfy fixed-register constraints. A
// value that has to survive the instruction, and an operand that isn't itself placed by that
// parallel copy, both have to stay out of these.
static U64 writtenRegisters(LowerBase base, LowerInst* inst, const InstShape& shape) {
    if(!shape.c) return 0;

    U64 mask = shape.isReturn ? 0 : shape.c->clobber;

    auto used = inst->used();
    for(Size i = 0; i < used.size(); i++) {
        mask |= regBit(wantForUse(base, inst, shape, i));
    }

    auto created = inst->created();
    for(Size i = 0; i < created.size(); i++) {
        mask |= regBit(wantForResult(inst, shape, i));
    }

    return mask;
}

/*
 * Parallel copies.
 */

// Sequences a set of simultaneous register-to-register copies into an order that executes them one
// at a time without any of them destroying a value another still has to read. A copy can be emitted
// as soon as nothing left in the set reads its destination; when nothing qualifies, what remains is
// a permutation cycle, and one exchange breaks it - x86 has `xchg`, so this needs no scratch
// register (and therefore can't fail for lack of one).
static void sequenceMoves(Array<RegMove>& pending, Array<RegMove>& out) {
    Array<bool> done;
    for(Size i = 0; i < pending.size(); i++) done.push(pending[i].from == pending[i].to);

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
            // Everything left is part of a cycle. Exchanging one copy's two ends satisfies it and
            // leaves the value that was in the destination sitting in the source, so whoever was
            // waiting to read the destination now has to read the source instead.
            Size i = 0;
            while(done[i]) i++;

            out.push(RegMove { pending[i].from, pending[i].to, true });
            done[i] = true;

            for(Size j = 0; j < pending.size(); j++) {
                if(!done[j] && pending[j].from == pending[i].to) pending[j].from = pending[i].from;
            }
        }
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
    U64 mask;
};

struct ValueInfo {
    LowerValue* value = nullptr;
    LiveRange range;

    // General registers this value may not be given, because something writes them while it is
    // live. Only meaningful for GenReg values - InstConstraints::clobber has no way to describe an
    // xmm clobber, which is why no calling convention can express one yet.
    U64 avoid = 0;

    RegId home = kInvalidReg;
};

struct Allocator {
    LowerBase base;
    LowerFunction& fun;
    Liveness& live;
    const Constraints& constraints;

    Array<ValueInfo> values;              // indexed by LiveId
    LiveId occupant[kRegClassCount][kRegCount];

    Allocator(LowerBase base, LowerFunction& fun, Liveness& live, const Constraints& constraints):
        base(base), fun(fun), live(live), constraints(constraints)
    {
        for(auto& cls: occupant) {
            for(auto& o: cls) o = kNullLive;
        }

        for(Size i = 0; i < live.valueMap.size(); i++) {
            values.push(ValueInfo { live.getValue(LiveId(i)), live.getRange(LiveId(i)) });
        }
    }

    ValueInfo& infoOf(LowerValue* v) {
        auto id = v->liveId();
        assertTrue(id != kNullLive); // every non-implicit value is numbered by buildLiveness
        return values[id];
    }

    // Where a value lives. Reading this before the value has been placed means the walk reached a
    // use before the definition, which the reverse-postorder block order rules out.
    RegId homeOf(LowerValue* v) {
        auto& info = infoOf(v);
        assertTrue(info.home != kInvalidReg);
        return info.home;
    }

    // A register is available to a value if nothing that outlives the start of that value's range
    // is sitting in it. Comparing against the range start (rather than against wherever the walk
    // currently is) is what lets a phi be placed at whichever predecessor edge reaches it first,
    // which is not necessarily the edge its range starts on.
    bool isFree(RegClass cls, Size index, U32 at) {
        auto o = occupant[cls][index];
        return o == kNullLive || values[o].range.end <= at;
    }

    // Places a value, preferring `hint` so that a copy the encoder would otherwise have to emit
    // collapses into a no-op.
    RegId assign(LowerValue* v, U64 extraAvoid, RegId hint) {
        auto id = v->liveId();
        assertTrue(id != kNullLive);

        auto& info = values[id];
        assertTrue(info.home == kInvalidReg); // a value is defined once

        auto cls = classForType(v->type);
        auto avoid = info.avoid | extraAvoid | (cls == GenReg ? kReservedRegs : 0);
        auto at = info.range.start;

        auto usable = [&](Size i) {
            if(cls == GenReg && (avoid & (U64(1) << i))) return false;
            return isFree(cls, i, at);
        };

        Size chosen = kRegCount;

        if(hint != kInvalidReg && getRegClass(hint) == cls && usable(getRegIndex(hint))) {
            chosen = getRegIndex(hint);
        } else {
            for(Size i = 0; i < kRegCount; i++) {
                if(usable(i)) { chosen = i; break; }
            }
        }

        // Spilling is not implemented - see the file comment.
        assertTrue(chosen < kRegCount); // register allocator ran out of registers

        info.home = makeRegId(cls, chosen);
        occupant[cls][chosen] = id;
        return info.home;
    }
};

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
            auto mask = writtenRegisters(a.base, inst, shape);

            if(mask) {
                // An operand that the parallel copy in front of this instruction does *not* place
                // is read straight out of its own register, so that register has to survive both
                // the copy and whatever the instruction's expansion writes before reading its
                // sources (`xor rdx, rdx` ahead of a division, r11 as scratch in an unrolled copy).
                auto used = inst->used();
                for(Size i = 0; i < used.size(); i++) {
                    auto v = a.base[used[i]];
                    if(isImplicit(v)) continue;
                    if(wantForUse(a.base, inst, shape, i) != kInvalidReg) continue;

                    a.infoOf(v).avoid |= mask;
                }

                // A return ends the function, so nothing can be live across it.
                if(!shape.isReturn) sites.push(ClobberSite { index, mask });
            }

            index++;
        };

        for(auto i: block->instructions.contents(a.base)) onInst(a.base[i]);
        onInst(a.base[block->terminator]);
    }

    for(auto& info: a.values) {
        if(info.range.isEmpty()) continue;

        for(auto& site: sites) {
            if(info.range.crosses(site.index)) info.avoid |= site.mask;
        }
    }
}

/*
 * Pass 2: place values and record, per instruction, where the encoder finds each operand.
 */

struct Emitter {
    Allocator& a;
    LowerBase base;

    explicit Emitter(Allocator& a): a(a), base(a.base) {}

    // Where the encoder reads operand `i`, given that the destructive destination (if any) has
    // already been placed. Used both to report operands and to keep a destructive result off the
    // registers its sibling operands are read from.
    RegId useLocation(LowerInst* inst, const InstShape& shape, Size i, RegId destructiveReg) {
        auto v = base[inst->used()[i]];
        if(isImplicit(v)) return kInvalidReg;

        auto want = wantForUse(base, inst, shape, i);
        if(want != kInvalidReg) return want;
        if(i == 0 && destructiveReg != kInvalidReg) return destructiveReg;

        return a.homeOf(v);
    }

    // The register a freshly defined value would rather have: the one its source operand is about
    // to vacate, so that the copy the encoder would emit becomes `mov r, r` and disappears.
    RegId copyHint(LowerInst* inst, U32 index) {
        auto used = inst->used();
        if(used.size() == 0) return kInvalidReg;

        auto source = base[used[0]];
        if(isImplicit(source)) return kInvalidReg;

        auto& info = a.infoOf(source);
        if(info.home == kInvalidReg || info.range.end > index) return kInvalidReg;

        return info.home;
    }

    InstRegs resolveInst(LowerInst* inst, U32 index) {
        InstRegs out;
        Array<RegMove> pending;

        auto shape = shapeOf(base, a.constraints, a.fun, inst);
        auto used = inst->used();
        auto created = inst->created();

        // The destructive destination has to be decided before anything else: it is where used()[0]
        // must sit by the time the instruction runs, so it is reported for both that operand and
        // the result. It must also avoid wherever the *other* operands are read from - the copy
        // that puts used()[0] there runs before the instruction, and would otherwise overwrite a
        // sibling operand that the instruction has not read yet.
        RegId destructiveReg = kInvalidReg;

        if(isDestructive(base, inst) && used.size() > 0 && created.size() > 0 && !isImplicit(&created[0])) {
            U64 blocked = 0;
            for(Size i = 1; i < used.size(); i++) {
                blocked |= regBit(useLocation(inst, shape, i, kInvalidReg));
            }

            destructiveReg = a.assign(&created[0], blocked, copyHint(inst, index));
        }

        for(Size i = 0; i < used.size(); i++) {
            auto v = base[used[i]];
            auto location = useLocation(inst, shape, i, destructiveReg);

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

            auto want = wantForResult(inst, shape, i);
            auto home = a.assign(&v, 0, want != kInvalidReg ? want : copyHint(inst, index));

            // A constrained result is produced in its fixed register and copied home afterwards.
            // Nothing live can be sitting there: the fixed register is part of this instruction's
            // written set, which every value whose range crosses the instruction avoids.
            if(want != kInvalidReg && want != home) {
                out.creates.push(want);
                out.postMoves.push(RegMove { want, home });
            } else {
                out.creates.push(home);
            }
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
            if(a.infoOf(&result).home == kInvalidReg) a.assign(&result, 0, a.infoOf(value).home);

            pending.push(RegMove { a.homeOf(value), a.homeOf(&result) });
        }
    }
};

// Places the incoming arguments and produces the copies, if any, that move them out of the
// registers the calling convention delivered them in. An argument that outlives a call can't stay
// in a register the call clobbers, so it is given a safe one and copied there on entry - once,
// rather than being shuffled at every call site.
static void assignArgs(Allocator& a, const InstConstraints& call, Array<RegMove>& entryMoves) {
    U32 index[kRegClassCount] = {};

    for(auto offset: a.fun.args.contents(a.base)) {
        auto& result = a.base[offset]->result;
        if(isImplicit(&result)) continue;

        auto cls = classForType(result.type);
        auto classIndex = index[cls];
        auto incoming = classIndex < kMaxRegInputs ? call.constraints[cls].args[classIndex] : kInvalidReg;

        if(incoming == kInvalidReg) {
            // Out of argument registers: the value arrives on the stack. Nothing downstream can
            // encode a stack operand yet, so this only gets as far as a clear failure there.
            a.infoOf(&result).home = makeRegId(StackReg, index[StackReg]);
            index[StackReg]++;
            continue;
        }

        index[cls]++;
        auto home = a.assign(&result, 0, incoming);
        if(home != incoming) entryMoves.push(RegMove { incoming, home });
    }
}

FunctionRegs allocateRegisters(Context& ctx, LowerBase base, LowerFunction& fun) {
    Constraints constraints;
    auto live = fun.buildLiveness(base);

    Allocator a(base, fun, *live, constraints);
    computeAvoidSets(a);

    Emitter emitter(a);
    FunctionRegs result;

    // Arguments occupy their registers from the entry point, so they are placed before anything
    // else can claim one. Their entry copies belong to the first thing the function executes.
    Array<RegMove> entryMoves;
    assignArgs(a, constraints.getCall(fun.callType), entryMoves);

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

    assertTrue(index == live->instCount); // the walk here and buildRanges' numbering must agree
    return result;
}
