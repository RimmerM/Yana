#include "lower_induction.h"
#include "lower_builder.h"

namespace {

// The scales an x86 SIB byte holds, and the same set an arm64 load-store offset shifts by. An
// address at one of these is free - `foldAddresses` puts the whole `base + index*scale` into the
// access that reads it - so reducing one would buy a live pointer register and an add per iteration
// in exchange for nothing. See the header: this is the gate that keeps the pass off the addresses
// the backend already handles, and the reason it needs the stride as a literal.
bool isEncodableScale(U64 scale) {
    return scale == 1 || scale == 2 || scale == 4 || scale == 8;
}

// Sixty-four bits wide, whether it is called an integer or an address. The index arithmetic below is
// only reducible at the width the address unit computes in: a 32-bit `shl` wraps where an address
// does not, which is the same reason `matchScaled` in the x64 backend declines one.
bool isWide(LowerType type) {
    return type == LowerType::Int64 || type == LowerType::Pointer;
}

U32 widthOf(LowerType type) {
    return type == LowerType::Int32 ? 32 : 64;
}

// A stored pattern read as the signed number of its own width, which is what a step has to be before
// anything at 64 bits multiplies it. The same routine lower_fold.cpp and lower_strength.cpp keep.
I64 signedValue(U64 value, U32 bits) {
    if(bits >= 64) return I64(value);

    auto spare = 64 - bits;
    return I64(value << spare) >> spare;
}

Maybe<U64> immediateOf(LowerBase base, LowerValue* value) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Imm || !isInt(value->type)) return Nothing();

    return Just(((LowerImm*)inst)->i);
}

/*
 * Whether a value is computed outside the loop, and so is the same on every iteration.
 *
 * Asked of the *block* rather than by walking operands, which is what makes an argument and a
 * literal invariant with no case of their own: both are defined in blocks the loop does not contain.
 *
 * A value that passes this also dominates the preheader, which is what lets the setup arithmetic go
 * there. It dominates the header, since something inside the loop reads it; every path into the
 * header runs through the preheader; so a definition that is not in the loop lies on the prefix of
 * that path, or is the preheader itself.
 */
bool outsideLoop(LowerBase base, const LoopInfo& loops, BlockIndex header, LowerValue* value) {
    auto block = value->inst()->block;
    if(!block) return false;

    return !loops.contains(header, base[block]->index);
}

// One loop in the shape this pass is stated in.
struct ReducibleLoop {
    LowerBlock* header;
    LowerBlock* pre;    // the one predecessor outside the loop
    LowerBlock* latch;  // the one predecessor inside it
};

/*
 * Whether the header dominates a block the rewrite is going to rely on it dominating.
 *
 * Asked rather than assumed, and the reason is in LoopInfo's own header: a natural loop's members are
 * dominated by its header only where the graph is reducible, and `buildLoops` says in as many words
 * that for an irreducible one its answer is "one possible reading of the loop rather than the only
 * one, which is all a heuristic needs". It is not all this needs - a phi placed in the header is read
 * where the header dominates the reader and nowhere else, and the failure of that is a value read
 * before it is defined rather than a worse decision.
 */
bool headerDominates(const ReducibleLoop& loop, const DominatorTree& dominators, LowerBlock* block) {
    return loop.header->dominates(block, dominators);
}

/*
 * A basic induction variable: a header phi advanced by a constant on the latch edge.
 *
 * `step` is what the latch adds, at 64 bits and read as an unsigned pattern - the pointer arithmetic
 * this builds is modular there, so a descending walk needs no case of its own.
 *
 * `widened` says the phi is *narrower* than the address unit and reaches it through a `sext`, which
 * is only an induction variable at all once the narrow addition has been proved not to wrap. See
 * `stepCannotOverflow`.
 */
struct Induction {
    LowerValue* initial;  // what the preheader hands over, at the phi's own width
    U64 step;
    bool widened;
};

// One address the loop recomputes, and everything needed to decide which pointer it becomes.
struct Candidate {
    LowerValue* address;  // the `add %base, %scaled` being replaced
    LowerValue* basePtr;
    LowerValue* initial;
    bool widened;         // whether the pointer's start has to sign-extend `initial` first
    U64 scale;
    U64 stride;
};

Maybe<ReducibleLoop> reducibleLoop(LowerBase base, const LoopInfo& loops,
                                   const DominatorTree& dominators, LowerBlock* header)
{
    if(header->incoming.size() != 2) return Nothing();

    auto first = base[header->incoming.get(base, 0)];
    auto second = base[header->incoming.get(base, 1)];

    auto firstInside = loops.contains(header->index, first->index);
    if(firstInside == loops.contains(header->index, second->index)) return Nothing();

    ReducibleLoop loop {};
    loop.header = header;
    loop.latch = firstInside ? first : second;
    loop.pre = firstInside ? second : first;

    // A preheader whose whole content is entering the loop, which is what the lowering produces and
    // what the x64 backend's loop rotation already requires of one. Setup arithmetic put in a block
    // that also leads elsewhere would run on a path that never enters the loop - harmless, since
    // everything built here is pure, but paid for nothing.
    if(!loop.pre->terminator || base[loop.pre->terminator]->kind != LowerInst::Jmp) return Nothing();
    if(!loop.latch->terminator) return Nothing();

    // The step on the back edge reads the phi in the header, so the header has to dominate the block
    // it is placed in - see headerDominates.
    if(!headerDominates(loop, dominators, loop.latch)) return Nothing();

    return Just(loop);
}

// The alternative a phi takes on the edge from `from`, or null where it has none.
LowerValue* phiValueFrom(LowerBase base, LowerInstPhi* phi, LowerBlock* from) {
    auto used = phi->used();
    auto sources = phi->sources();

    for(Size i = 0; i < used.size(); i++) {
        if(base[sources[i]] == from) return base[used[i]];
    }

    return nullptr;
}

// A comparison's own operands, where the value is one.
LowerInstCmp* comparisonOf(LowerBase base, LowerValue* value) {
    auto inst = value->inst();
    return inst->kind == LowerInst::Cmp ? (LowerInstCmp*)inst : nullptr;
}

// Whether a comparison holds `%phi` on the left against something, and what that something is.
LowerValue* comparedAgainst(LowerBase base, LowerInstCmp* cmp, LowerValue* of) {
    if(!cmp || base[cmp->lhs] != of) return nullptr;
    return base[cmp->rhs];
}

// `sub %a, %b`, for the distance a range loop's guard is stated in.
bool isDifference(LowerBase base, LowerValue* value, LowerValue* of, LowerValue* bound, bool ofFirst) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Sub || value->type != of->type) return false;

    auto binary = (LowerInstBinary*)inst;
    auto lhs = base[binary->lhs];
    auto rhs = base[binary->rhs];

    return ofFirst ? (lhs == of && rhs == bound) : (lhs == bound && rhs == of);
}

/*
 * The second no-wrap proof, and the one an ordinary `while` loop can offer - §14.5 item 1 of
 * test/bench/findings.md.
 *
 * The proof below this one reads the guard a `for` range emits. A `while i < n` has no such guard,
 * and was declined for it - which is what that file calls the `sext` wall, and what kept every
 * hand-written scan loop out of both this pass and the widening beside it. But the loop's own test is
 * evidence in its own right whenever it is the *strict* one and the step is exactly one:
 *
 *     %i < %B  ⟹  %i <= %B - 1  ⟹  %i + 1 <= %B <= INT_MAX
 *
 * so the addition lands at or below the largest value of the counter's own type and cannot wrap. Both
 * halves are load-bearing. `<=` gives `%i + 1 <= %B + 1`, which is exactly one past what the type
 * holds; and a step of two gives `%i + 2 <= %B + 1`, the same. Neither is provable without a bound on
 * `%B`, and nothing here provides one.
 *
 * Descending is the mirror: `%i > %B` with a step of minus one gives `%i - 1 >= %B >= INT_MIN`.
 *
 * The header test is a fact *about this iteration* because the step is inside the loop. Every path
 * that reaches a block in the loop passed through the header - it is the loop's only entry - and the
 * arm that leaves the header for the loop is the one the test was true on. The counter is a header
 * phi, so its value did not change in between.
 */
bool strictTestCannotOverflow(LowerBase base, const LoopInfo& loops, const ReducibleLoop& loop,
                              LowerInstPhi* phi, LowerInst* stepInst, I64 amount, bool ascending)
{
    if(amount != 1) return false;
    if(!loops.contains(loop.header->index, base[stepInst->block]->index)) return false;

    auto headerJe = base[loop.header->terminator];
    if(headerJe->kind != LowerInst::Je) return false;

    auto branch = (LowerInstJe*)headerJe;
    if(!loops.contains(loop.header->index, base[branch->then]->index)) return false;
    if(loops.contains(loop.header->index, base[branch->otherwise]->index)) return false;

    auto continues = comparisonOf(base, base[branch->cond]);
    auto bound = comparedAgainst(base, continues, &phi->result);
    if(!bound || bound == &phi->result || bound->type != phi->result.type) return false;

    return continues->getCmp() == (ascending ? LowerCmp::ilt : LowerCmp::igt);
}

/*
 * Whether a narrow counter provably does not wrap, which is the whole of what widening it needs.
 *
 * `doc/spec/expressions.md` states this as a language rule for the `for` range forms — "no bound and
 * no step may overflow on the way out of the loop. The emitted test compares the counter's *distance*
 * from the far end against the stride rather than comparing the next value against the bound, so a
 * loop that ends at the top of its type stops instead of wrapping past it." That is the fact this
 * pass needs, and it is the one thing resolve knows that the lower IR does not say.
 *
 * It does not have to be carried down, though, because the guard that *makes* it true is emitted
 * control flow and is still standing here. So it is checked rather than believed, which is the
 * stronger of the two: a flag set in resolve would be a claim nothing downstream could verify after
 * eight rounds of rewriting, while the guard is evidence at the point of use. A `while` loop with a
 * hand-written counter has no such guard and is declined — correctly, since `doc/spec/types.md` says
 * integer overflow wraps, and marks even that provisional.
 *
 * ## The proof
 *
 * The step block is entered only along the false arm of `je (cmp_ile %d, %s), exit, step`, where `%d`
 * is the distance to the far end and `%s` the stride; and the header, which dominates it, has already
 * established the loop's own test on the same bound. Ascending, with `%d = %B - %i` at the counter's
 * width and `%s > 0`:
 *
 *  - where `%B - %i` does not itself overflow, `%d` is the true difference, and `%d > %s` gives
 *    `%i + %s < %B`, which is at most the type's largest value. So the addition cannot overflow.
 *  - where it does overflow, `%d` came out negative — the header established `%i < %B`, so the true
 *    difference is positive and can only wrap by exceeding the signed maximum — and a negative `%d`
 *    is not greater than a positive `%s`, so the step is not taken at all.
 *
 * Descending is the mirror with `%d = %i - %B` and a subtraction, and `%i - %s > %B` is at least the
 * type's smallest value. `n downto 0` folds its `sub %i, 0` away, so the distance is the counter
 * itself and the bound is a literal zero; that is the same statement with the subtraction spent.
 *
 * Both header tests are accepted in their inclusive and exclusive spellings, since `..=` only moves
 * which of `<` and `<=` the header asks and the argument above reads the same either way.
 */
bool stepCannotOverflow(LowerBase base, const LoopInfo& loops, const ReducibleLoop& loop,
                        LowerInstPhi* phi, LowerInst* stepInst, LowerValue* stride, bool ascending)
{
    // A positive stride, and one this can read. Both directions state it as a magnitude - the
    // subtraction is what makes a descending loop descend - so a negative one would invert the
    // comparison below rather than merely reversing the walk.
    auto amount = immediateOf(base, stride);
    if(!amount || amount.unwrap() == 0) return false;
    if(stride->type != phi->result.type) return false;
    if(I64(signedValue(amount.unwrap(), widthOf(phi->result.type))) <= 0) return false;

    // The second proof, which needs no guard at all - see `strictTestCannotOverflow`. It is tried
    // first because it is the cheaper question and the one an ordinary `while` loop answers.
    if(strictTestCannotOverflow(base, loops, loop, phi, stepInst,
                                signedValue(amount.unwrap(), widthOf(phi->result.type)), ascending))
    {
        return true;
    }

    // The step runs on exactly one edge, and it is the one the guard lets through.
    auto stepBlock = base[stepInst->block];
    if(stepBlock->incoming.size() != 1) return false;

    auto guard = base[stepBlock->incoming.get(base, 0)];
    if(!guard->terminator || base[guard->terminator]->kind != LowerInst::Je) return false;

    auto je = (LowerInstJe*)base[guard->terminator];
    if(base[je->otherwise] != stepBlock || base[je->then] == stepBlock) return false;

    // `%d <= %s` leaves the loop, so the step sees `%d > %s`; `<` leaves it one earlier and gives
    // `%d >= %s`, which the argument above reads the same way.
    auto test = comparisonOf(base, base[je->cond]);
    if(!test) return false;
    if(test->getCmp() != LowerCmp::ile && test->getCmp() != LowerCmp::ilt) return false;
    if(base[test->rhs] != stride) return false;

    // And the loop's own test, on the same bound, taken on the arm that stays inside - which is what
    // makes it a fact the guard below it may be read against.
    auto headerJe = base[loop.header->terminator];
    if(headerJe->kind != LowerInst::Je) return false;

    auto branch = (LowerInstJe*)headerJe;
    if(!loops.contains(loop.header->index, base[branch->then]->index)) return false;
    if(loops.contains(loop.header->index, base[branch->otherwise]->index)) return false;

    auto continues = comparisonOf(base, base[branch->cond]);
    auto bound = comparedAgainst(base, continues, &phi->result);
    if(!bound || bound->type != phi->result.type) return false;

    auto distance = base[test->lhs];
    auto direction = continues->getCmp();

    if(ascending) {
        if(direction != LowerCmp::ilt && direction != LowerCmp::ile) return false;
        return isDifference(base, distance, &phi->result, bound, false);
    }

    if(direction != LowerCmp::igt && direction != LowerCmp::ige) return false;
    if(isDifference(base, distance, &phi->result, bound, true)) return true;

    // The folded form: `n downto 0` subtracts nothing, so the distance is the counter and the bound
    // it was measured from is the literal the header compares against.
    auto zero = immediateOf(base, bound);
    return distance == &phi->result && zero && zero.unwrap() == 0;
}

/*
 * Whether a header phi counts by a constant.
 *
 * The value it takes on the latch edge has to be `%phi + C`. Nothing has to be checked about *where*
 * that addition sits: it is not a phi and it is live on the latch's outgoing edge, so its block
 * dominates the latch and it therefore runs exactly once per iteration - which is the whole of what
 * "advanced by a constant" needs.
 *
 * A counter narrower than the address unit is admitted only where `stepCannotOverflow` proves the
 * narrow addition cannot wrap, since that is exactly what makes `sext(%i + C) == sext(%i) + C` - and
 * so what makes the widened sequence an induction variable rather than a different sequence that
 * usually agrees.
 */
Maybe<Induction> inductionOf(LowerBase base, const LoopInfo& loops, const ReducibleLoop& loop,
                             LowerInstPhi* phi)
{
    if(phi->usedCount != 2) return Nothing();
    if(!isInt(phi->result.type)) return Nothing();

    auto narrow = !isWide(phi->result.type);

    auto initial = phiValueFrom(base, phi, loop.pre);
    auto advanced = phiValueFrom(base, phi, loop.latch);
    if(!initial || !advanced || advanced == &phi->result) return Nothing();
    if(advanced->type != phi->result.type) return Nothing();

    auto inst = advanced->inst();
    if(inst->kind != LowerInst::Add && inst->kind != LowerInst::Sub) return Nothing();

    auto binary = (LowerInstBinary*)inst;
    auto lhs = base[binary->lhs];
    auto rhs = base[binary->rhs];
    auto bits = widthOf(phi->result.type);

    LowerValue* stride = nullptr;
    U64 step;

    if(lhs == &phi->result) {
        auto constant = immediateOf(base, rhs);
        if(!constant) return Nothing();

        // Sign-extended out of its own width before anything multiplies it: a `-1` stored at `Int32`
        // is `0xffffffff`, and a stride of four billion is not a stride of minus one.
        auto signedStep = signedValue(constant.unwrap(), bits);
        step = inst->kind == LowerInst::Sub ? U64(0) - U64(signedStep) : U64(signedStep);
        stride = rhs;
    } else if(rhs == &phi->result && inst->kind == LowerInst::Add) {
        // `add C, %i` as well as `add %i, C`: the operand canonicalization that settles which side a
        // literal is on belongs to the x64 backend, and has not run.
        auto constant = immediateOf(base, lhs);
        if(!constant) return Nothing();

        step = U64(signedValue(constant.unwrap(), bits));
        stride = lhs;
    } else {
        return Nothing();
    }

    if(step == 0) return Nothing();

    if(narrow && !stepCannotOverflow(base, loops, loop, phi, inst, stride,
                                     inst->kind == LowerInst::Add))
    {
        return Nothing();
    }

    return Just(Induction { initial, step, narrow });
}

// The factor a value scales `of` by, where it is `%of << k` or `%of * f` at a literal - the two
// spellings a stride reaches here as. `lower_strength.cpp` has already turned a multiply by a power
// of two into a shift; both remain because a stride like 24 is neither.
Maybe<U64> scaleOf(LowerBase base, LowerValue* value, LowerValue* of) {
    auto inst = value->inst();
    if(!isBinary(inst) || !isWide(value->type)) return Nothing();

    auto binary = (LowerInstBinary*)inst;
    if(base[binary->lhs] != of) return Nothing();

    auto constant = immediateOf(base, base[binary->rhs]);
    if(!constant) return Nothing();

    if(inst->kind == LowerInst::Shl) {
        if(constant.unwrap() > 62) return Nothing();
        return Just(U64(1) << constant.unwrap());
    }

    if(inst->kind == LowerInst::Mul || inst->kind == LowerInst::IMul) {
        if(constant.unwrap() == 0) return Nothing();
        return constant;
    }

    return Nothing();
}

// The base an address adds a scaled index to, or null where it is not that shape. A pointer result
// is required rather than assumed: a 32-bit add of an index to something is index arithmetic and
// wraps at a width the pointer built from it does not.
LowerValue* addressBaseOf(LowerBase base, LowerValue* address, LowerValue* scaled) {
    auto inst = address->inst();
    if(inst->kind != LowerInst::Add || !isPtr(address->type)) return nullptr;

    auto binary = (LowerInstBinary*)inst;
    auto lhs = base[binary->lhs];
    auto rhs = base[binary->rhs];

    // `add %o, %o` reads the index twice and adds no base at all.
    if(lhs == rhs) return nullptr;
    if(lhs == scaled) return rhs;
    if(rhs == scaled) return lhs;

    return nullptr;
}

// Whether every reader of a value is inside the loop, and reads it where the header's own phi is
// available. The pointer that replaces it holds, once the loop is over, whatever the last *entry to
// the header* put there rather than what the last iteration computed, and an address built under a
// branch makes those two different - so a reader outside is declined rather than reasoned about.
bool readOnlyInsideLoop(LowerBase base, const LoopInfo& loops, const ReducibleLoop& loop,
                        const DominatorTree& dominators, LowerValue* value)
{
    if(value->uses.isEmpty()) return false;

    for(auto u: value->uses.contents(base)) {
        auto block = base[u]->block;
        if(!block) return false;
        if(!loops.contains(loop.header->index, base[block]->index)) return false;
        if(!headerDominates(loop, dominators, base[block])) return false;
    }

    return true;
}

// Whether an instruction that nothing reads may simply go. The list is what this pass orphans:
// everything on it computes a value and does nothing else.
bool isRemovableArithmetic(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Imm:
        case LowerInst::Set:
        case LowerInst::Cast:
        case LowerInst::Bitcast:
        case LowerInst::Neg:
        case LowerInst::Not:
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            return inst->createdCount == 1;
        default:
            return false;
    }
}

/*
 * The instructions this pass orphaned, taken back out.
 *
 * `removeDeadConstants` sweeps immediates and nothing else, and nothing below this stage removes an
 * arithmetic instruction that has stopped being read - so a replaced address, and the shift that fed
 * it, would otherwise reach the backend with no reader at all. Collected as a set and swept per
 * block, because that is the one shape a rewrite of a LowerList takes here: rebuild the list from a
 * local array rather than removing from it in place.
 */
struct DeadSweep {
    LowerBase base;
    LowerModule& module;
    SmallArray<LowerPtr<LowerInst>, 16> dead;

    void kill(LowerInst* inst) {
        auto pointer = inst - base;
        if(dead.containsValue(pointer)) return;

        dead.push(pointer);

        // What it read is collected before it stops reading them, so that whichever of them this was
        // the last reader of becomes a candidate in its own right.
        SmallArray<LowerValue*, 4> operands;
        for(auto use: inst->used()) operands.push(base[use]);

        detach(base, inst);

        for(auto operand: operands) {
            auto producer = operand->inst();
            if(!producer->block || !isRemovableArithmetic(producer)) continue;
            if(operand->uses.isNotEmpty()) continue;

            kill(producer);
        }
    }

    void sweep(LowerFunction& fun) {
        if(dead.isEmpty()) return;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            // Inline: one of these per block, holding the instructions of that block while its list
            // is rebuilt - the same shape foldFunctionConstants uses, and for the same reason.
            SmallArray<LowerPtr<LowerInst>, 32> kept;
            auto dropped = false;

            for(auto instPtr: block->instructions.contents(base)) {
                if(dead.containsValue(instPtr)) {
                    dropped = true;
                    continue;
                }

                kept.push(instPtr);
            }

            if(!dropped) continue;

            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
        }
    }
};

// An unattached phi with room for two alternatives, filled in and added to a block by the caller -
// the same shape lower_promote.cpp and the x64 rotation build one in, because adding it is what
// registers its reads.
LowerInstPhi* makeInductionPhi(Region<LowerRegion>& arena, LowerType type) {
    auto storage = arena.alloc(
        sizeof(LowerInstPhi) +
        sizeof(LowerPtr<LowerValue>) * 2 +
        sizeof(LowerPtr<LowerBlock>) * 2);

    auto phi = new (storage) LowerInstPhi(StringId(), type);
    phi->usedCount = 2;
    return phi;
}

// The single value an instruction the builders handed back produces. Every builder used below either
// creates a one-result instruction or forwards an operand's, and `foldBinary` only forwards where
// the producer has exactly one result - so this is total rather than a check.
LowerValue* resultOf(LowerInst* inst) {
    assertTrue(inst->createdCount == 1);
    return inst->created().ptr;
}

LowerValue* immediate(LowerBase base, LowerModule& module, LowerBlock& block, LowerType type, U64 value) {
    return resultOf(block.addInst(base, new (module.arena) LowerImm(StringId(), type, value)));
}

/*
 * One group of addresses, replaced by a pointer the loop carries.
 *
 * The preheader gets what the first iteration would have computed, which for the usual counter
 * starting at zero folds all the way back to the base itself; the latch gets one add of the stride;
 * and the header gets the phi that merges them. Nothing is inserted where the addresses were - they
 * are replaced by the phi's result, which the header dominates.
 */
void reduceGroup(LowerBase base, LowerModule& module, LowerFunction& fun, const ReducibleLoop& loop,
                 const Candidate& shape, SmallArray<Candidate, 8>& candidates, DeadSweep& sweep)
{
    auto& arena = fun.arena;

    // A narrow counter's start has to arrive at the address unit's width the same way the counter
    // itself does inside the loop - through a sign extension. Which for the usual literal zero is
    // one the fold answers on the spot.
    auto initial = shape.widened
        ? resultOf(cast<true, true>(base, module, *loop.pre, shape.initial, LowerType::Int64, StringId()))
        : shape.initial;

    auto scaleImm = immediate(base, module, *loop.pre, LowerType::Int64, shape.scale);
    auto offset = binary<LowerInst::Mul>(base, module, *loop.pre, initial, scaleImm,
                                         LowerType::Int64, StringId());

    auto start = binary<LowerInst::Add>(base, module, *loop.pre, shape.basePtr, resultOf(offset),
                                        LowerType::Pointer, StringId());

    auto phi = makeInductionPhi(arena, LowerType::Pointer);
    phi->source = shape.address->inst()->source;

    auto strideImm = immediate(base, module, *loop.latch, LowerType::Int64, shape.stride);
    auto advanced = binary<LowerInst::Add>(base, module, *loop.latch, &phi->result, strideImm,
                                           LowerType::Pointer, StringId());

    auto used = phi->used();
    auto sources = phi->sources();

    used[0] = resultOf(start) - base;
    sources[0] = loop.pre - base;
    used[1] = resultOf(advanced) - base;
    sources[1] = loop.latch - base;

    loop.header->addInst(base, phi);

    for(auto& candidate: candidates) {
        if(candidate.basePtr != shape.basePtr) continue;
        if(candidate.initial != shape.initial || candidate.widened != shape.widened) continue;
        if(candidate.scale != shape.scale || candidate.stride != shape.stride) continue;

        replaceUses(base, arena, candidate.address - base, &phi->result - base);
        sweep.kill(candidate.address->inst());
    }
}

/*
 * Every address one scaled index feeds, where all of them qualify.
 *
 * All of them, because that is what makes the rewrite pay: the shift dies only once nothing reads it,
 * and a shift left standing beside a pointer that now advances is the same arithmetic as before plus
 * a live register. The addresses need not share a base - one index scaled into two arrays becomes two
 * pointers and no shift, which is still one instruction fewer.
 */
bool collectScaledAddresses(LowerBase base, const LoopInfo& loops, const ReducibleLoop& loop,
                            const DominatorTree& dominators, LowerValue* scaled,
                            const Induction& counter, U64 scale, SmallArray<Candidate, 8>& into)
{
    if(scaled->uses.isEmpty()) return false;

    auto header = loop.header->index;
    auto first = into.size();

    for(auto u: scaled->uses.contents(base)) {
        auto inst = base[u];
        if(inst->createdCount != 1) return false;

        auto address = inst->created().ptr;
        auto pointer = addressBaseOf(base, address, scaled);
        if(!pointer) return false;
        if(!outsideLoop(base, loops, header, pointer)) return false;

        // And it has to reach the preheader, where the pointer's start is computed. Implied by the
        // line above wherever the graph is reducible, and asked outright because that is exactly
        // what LoopInfo does not promise.
        if(!base[pointer->inst()->block]->dominates(loop.pre, dominators)) return false;
        if(!readOnlyInsideLoop(base, loops, loop, dominators, address)) return false;

        into.push(Candidate { address, pointer, counter.initial, counter.widened, scale,
                              counter.step * scale });
    }

    // A shift read twice by the same address is not two addresses, and `addressBaseOf` has already
    // refused that shape - so anything reaching here is a distinct address per use.
    return into.size() > first;
}

void reduceLoop(LowerBase base, LowerModule& module, LowerFunction& fun, const LoopInfo& loops,
                const DominatorTree& dominators, const ReducibleLoop& loop, DeadSweep& sweep)
{
    auto header = loop.header->index;

    // Snapshotted before anything is built, because what this pass adds to the header is itself a
    // header phi, and one with no latch step to find.
    SmallArray<LowerInstPhi*, 8> counters;
    for(auto p: loop.header->phis.contents(base)) counters.push(base[p]);

    SmallArray<Candidate, 8> candidates;

    for(auto phi: counters) {
        auto induction = inductionOf(base, loops, loop, phi);
        if(!induction) continue;

        auto& counter = induction.unwrap();

        // The counter's start has to reach the preheader, where the pointer's start is computed. It
        // does by construction - it is the alternative the preheader edge carries - but the check
        // states it, since a value defined inside the loop is one the setup cannot read.
        if(!outsideLoop(base, loops, header, counter.initial)) continue;

        /*
         * What an address scales. A counter already at the address unit's width is that value; a
         * narrow one reaches an address only through a sign extension, and there is one of those per
         * subscript rather than one per loop - `xs[i].a` and `xs[i].c` each widen the counter for
         * themselves, and nothing below this stage does CSE.
         *
         * Every one of them stands for the same sequence, so they produce candidates that group
         * together and end up sharing one pointer.
         */
        SmallArray<LowerValue*, 4> indexes;

        if(!counter.widened) {
            indexes.push(&phi->result);
        } else {
            for(auto u: phi->result.uses.contents(base)) {
                auto widening = base[u];
                if(widening->kind != LowerInst::Cast || widening->createdCount != 1) continue;

                // Sign-extending, and only that: a `zext` steps by the same constant only where the
                // counter is known not to be negative, which the no-wrap proof does not say.
                auto conversion = (LowerInstCast*)widening;
                if(!conversion->isSignedSource() || !conversion->isSignedResult()) continue;
                if(!isWide(conversion->result.type) || !isInt(conversion->result.type)) continue;

                indexes.push(&conversion->result);
            }
        }

        for(auto index: indexes) {
            for(auto scaledUse: index->uses.contents(base)) {
                auto scaledInst = base[scaledUse];
                if(scaledInst->createdCount != 1) continue;

                auto scaled = scaledInst->created().ptr;
                auto factor = scaleOf(base, scaled, index);
                if(!factor || isEncodableScale(factor.unwrap())) continue;

                SmallArray<Candidate, 8> found;
                if(!collectScaledAddresses(base, loops, loop, dominators, scaled, counter,
                                           factor.unwrap(), found))
                {
                    continue;
                }

                for(auto& candidate: found) candidates.push(candidate);
            }
        }
    }

    // One pointer per distinct base, start, scale and stride: `xs[i].a` and `xs[i].d` are two adds of
    // the same two values, and both become the same phi with the field offsets left as the
    // displacements they already were.
    SmallArray<Candidate, 8> reduced;

    for(auto& candidate: candidates) {
        auto already = false;
        for(auto& done: reduced) {
            if(done.basePtr == candidate.basePtr && done.initial == candidate.initial &&
               done.widened == candidate.widened &&
               done.scale == candidate.scale && done.stride == candidate.stride)
            {
                already = true;
                break;
            }
        }

        if(already) continue;

        reduced.push(candidate);
        reduceGroup(base, module, fun, loop, candidate, candidates, sweep);
    }
}

/*
 * A narrow counter and everything that has to change with it, for the widening below.
 *
 * `extensions` are the sign extensions that become the wide counter itself, and `comparisons` the
 * tests that have to be re-asked at the wider width. Between them and the step they must account for
 * every reader the counter has: one left over is a reader of a value that will no longer exist, and
 * narrowing the wide counter back for it would spend the instruction this is trying to remove.
 */
struct NarrowCounter {
    LowerInstPhi* phi;
    LowerInst* step;
    LowerValue* initial;
    U64 amount;

    SmallArray<LowerInst*, 4> extensions;
    SmallArray<LowerInstCmp*, 4> comparisons;
};

// Whether sign-extending both operands of a comparison leaves it asking the same question. The
// signed orderings and the two equalities do; the unsigned orderings do not, since sign extension is
// exactly what changes which of two patterns is the larger unsigned number.
bool survivesWidening(LowerCmp kind) {
    switch(kind) {
        case LowerCmp::eq:
        case LowerCmp::neq:
        case LowerCmp::igt:
        case LowerCmp::ige:
        case LowerCmp::ilt:
        case LowerCmp::ile:
            return true;
        default:
            return false;
    }
}

// The operand of a comparison that is not the counter, where exactly one of them is.
LowerValue* otherOperand(LowerBase base, LowerInstCmp* cmp, LowerValue* of) {
    auto lhs = base[cmp->lhs];
    auto rhs = base[cmp->rhs];

    if((lhs == of) == (rhs == of)) return nullptr;
    return lhs == of ? rhs : lhs;
}

/*
 * Every reader of a narrow counter, sorted into the three kinds the widening can carry.
 *
 * The step is the one that does not have to be checked for anything: `inductionOf` found it, and its
 * own result is checked to have exactly one reader - the phi - because the wide chain replaces both
 * and a second reader of the advanced value would be left naming a deleted one.
 */
bool classifyCounter(LowerBase base, const LoopInfo& loops, BlockIndex header, NarrowCounter& counter) {
    auto advanced = counter.step->created().ptr;
    if(advanced->uses.size() != 1) return false;

    for(auto u: counter.phi->result.uses.contents(base)) {
        auto inst = base[u];
        if(inst == counter.step) continue;

        if(inst->kind == LowerInst::Cast && inst->createdCount == 1) {
            auto conversion = (LowerInstCast*)inst;

            // Sign-extending to the wide width, and only that: a zero extension of a counter this
            // pass is about to make signed-wide is a different number wherever the counter is
            // negative, which the no-wrap proof says nothing about.
            if(conversion->isSignedSource() && conversion->isSignedResult() &&
               conversion->result.type == LowerType::Int64)
            {
                counter.extensions.push(inst);
                continue;
            }

            return false;
        }

        if(inst->kind == LowerInst::Cmp) {
            auto cmp = (LowerInstCmp*)inst;
            if(!survivesWidening(cmp->getCmp())) return false;

            auto other = otherOperand(base, cmp, &counter.phi->result);
            if(!other || other->type != counter.phi->result.type) return false;

            // Re-asked in the preheader, so the widened operand has to be computable there. Every
            // block that can read the counter's phi is dominated by the header and so by the
            // preheader, which is what makes one sign extension serve every comparison.
            if(!outsideLoop(base, loops, header, other)) return false;

            counter.comparisons.push(cmp);
            continue;
        }

        return false;
    }

    // Nothing is bought by widening a counter no address widens: the arithmetic is the same
    // arithmetic one register class up, and on x64 it is the same arithmetic with a REX byte.
    return counter.extensions.isNotEmpty();
}

/*
 * The counter, widened - §14.5 item 1 and 3 of test/bench/findings.md, arrived at without item 2.
 *
 * `hashOf` in test/bench/programs/Text.yana is the case the item is written about: seven instructions
 * in its byte loop against `-Os`'s six, and the extra one is the `movslq` that widens the index for
 * the address. The item costed removing it as a pointer induction variable plus a down-counter to
 * take the index's last reader away, and noted that neither pays without the other. Widening the
 * counter itself is a third answer that needs neither: the sign extension stops existing because
 * there is nothing left to extend, and the address, the step and the test are the same three
 * instructions they always were one width up.
 *
 *     %i  = phi [pre, 0], [body, %i2]      %i = phi [pre, 0], [body, %i2]  : Long
 *     %w  = sext %i                   ->   %p = add %base, %i
 *     %p  = add %base, %w                  %i2 = add %i, 1
 *     %i2 = add %i, 1                      %c = cmp_ilt %i, %n64
 *     %c  = cmp_ilt %i, %n
 *
 * What makes it legal is the no-wrap proof and nothing else: the wide sequence is the sign extension
 * of the narrow one exactly when the narrow one does not wrap, which is what `stepCannotOverflow`
 * answers and what §14.5 item 1 widened to reach a `while` loop at all.
 *
 * After `reduceInductionVariables` rather than before it. A sign extension that pass is going to
 * delete - the index of a stride the SIB byte cannot hold, which becomes a pointer the loop carries -
 * is not one this should widen a counter for, and running behind it is what makes "is there a sign
 * extension left" the right question rather than a guess.
 */
void widenLoopCounters(LowerBase base, LowerModule& module, LowerFunction& fun, const LoopInfo& loops,
                       const ReducibleLoop& loop, DeadSweep& sweep)
{
    auto& arena = fun.arena;
    auto header = loop.header->index;

    SmallArray<LowerInstPhi*, 8> phis;
    for(auto p: loop.header->phis.contents(base)) phis.push(base[p]);

    for(auto phi: phis) {
        auto induction = inductionOf(base, loops, loop, phi);
        if(!induction) continue;

        auto& found = induction.unwrap();
        if(!found.widened) continue;   // already at the address unit's width
        if(!outsideLoop(base, loops, header, found.initial)) continue;

        NarrowCounter counter;
        counter.phi = phi;
        counter.step = phiValueFrom(base, phi, loop.latch)->inst();
        counter.initial = found.initial;
        counter.amount = found.step;

        if(!classifyCounter(base, loops, header, counter)) continue;

        /*
         * The wide chain, built beside the narrow one and then put in its place.
         *
         * The step is appended to the block the narrow one is in, which is where its own operands
         * are and where it already runs once per iteration. The initial value is the preheader's,
         * sign-extended - a literal zero folds all the way away.
         */
        auto initial = resultOf(cast<true, true>(base, module, *loop.pre, counter.initial,
                                                 LowerType::Int64, StringId()));

        auto wide = makeInductionPhi(arena, LowerType::Int64);
        wide->source = phi->source;

        auto stepBlock = base[counter.step->block];
        auto stepImm = immediate(base, module, *stepBlock, LowerType::Int64, counter.amount);
        auto stepped = binary<LowerInst::Add>(base, module, *stepBlock, &wide->result, stepImm,
                                              LowerType::Int64, StringId());

        auto used = wide->used();
        auto sources = wide->sources();

        used[0] = initial - base;
        sources[0] = loop.pre - base;
        used[1] = resultOf(stepped) - base;
        sources[1] = loop.latch - base;

        loop.header->addInst(base, wide);

        // The extensions are what the wide counter *is*, so their readers read it directly.
        for(auto extension: counter.extensions) {
            replaceUses(base, arena, extension->created().ptr - base, &wide->result - base);
            sweep.kill(extension);
        }

        // And the tests, re-asked one width up. The bound is widened once per comparison rather than
        // once per loop; `eliminateCommonValues` has already run, so two of them would not be
        // unified - but two comparisons against one bound is not a shape the front end emits.
        for(auto cmp: counter.comparisons) {
            auto other = otherOperand(base, cmp, &phi->result);
            auto widened = resultOf(cast<true, true>(base, module, *loop.pre, other,
                                                     LowerType::Int64, StringId()));

            auto counterFirst = base[cmp->lhs] == &phi->result;
            setOperand(base, arena, cmp, counterFirst ? cmp->lhs : cmp->rhs, &wide->result);
            setOperand(base, arena, cmp, counterFirst ? cmp->rhs : cmp->lhs, widened);
        }

        // Nothing reads the narrow counter now. The phi goes by hand - it is not in the instruction
        // list the sweep rebuilds - and the step follows it as ordinary dead arithmetic.
        detach(base, (LowerInst*)phi);

        for(Size i = 0; i < loop.header->phis.size(); i++) {
            if(base[loop.header->phis.get(base, i)] != phi) continue;
            loop.header->phis.remove(base, i);
            break;
        }

        sweep.kill(counter.step);
    }
}

} // namespace

void reduceInductionVariables(LowerBase base, LowerModule& module, LowerFunction& fun) {
    if(fun.blocks.size() < 2) return;

    // Both valid for the whole walk: nothing below creates, removes or renumbers a block, so the loop
    // structure and the dominator tree the first two lines find are the ones the last line reduces
    // against - which is also what keeps `postIndex`, the tree's index, standing.
    auto loops = fun.buildLoops(base);
    auto dominators = fun.buildDominatorTree(base);
    DeadSweep sweep { base, module, {} };

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(!loops.isHeader(block->index)) continue;

        if(auto loop = reducibleLoop(base, loops, dominators, block)) {
            reduceLoop(base, module, fun, loops, dominators, loop.unwrap(), sweep);
        }
    }

    sweep.sweep(fun);
}

void widenInductionVariables(LowerBase base, LowerModule& module, LowerFunction& fun) {
    if(fun.blocks.size() < 2) return;

    // The same two answers the reduction above reads, and valid for the same reason: nothing here
    // creates, removes or renumbers a block either.
    auto loops = fun.buildLoops(base);
    auto dominators = fun.buildDominatorTree(base);
    DeadSweep sweep { base, module, {} };

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(!loops.isHeader(block->index)) continue;

        if(auto loop = reducibleLoop(base, loops, dominators, block)) {
            widenLoopCounters(base, module, fun, loops, loop.unwrap(), sweep);
        }
    }

    sweep.sweep(fun);
}
