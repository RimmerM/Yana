#include "lower_induction.h"
#include "lower_builder.h"
#include "lower_fold.h"

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
 * A basic induction variable: a header phi advanced by the same amount on every latch edge.
 *
 * `step` is what the latch adds, at 64 bits and read as an unsigned pattern - the pointer arithmetic
 * this builds is modular there, so a descending walk needs no case of its own.
 *
 * `variable` is the other kind of step, and it is null for the ordinary one: a value the loop does
 * not compute, added to the counter every iteration. §32 - `m = m + p` in the marking loop of
 * test/bench/programs/Sieve.yana, where `p` is the prime being sieved. It is still an induction
 * variable and the address it drives is still a pointer the loop can carry; what changes is that the
 * amount the pointer advances by has to be multiplied out in the preheader rather than written as an
 * immediate in the latch.
 *
 * A variable step is only ever taken at the address unit's own width, and that falls out of the
 * proof rather than being a rule of its own: a narrow counter needs `stepCannotOverflow`, which reads
 * the stride as a number, so a narrow counter with a runtime step is refused before it gets here.
 *
 * `widened` says the phi is *narrower* than the address unit and reaches it through a `sext`, which
 * is only an induction variable at all once the narrow addition has been proved not to wrap. See
 * `stepCannotOverflow`.
 */
struct Induction {
    LowerValue* initial;   // what the preheader hands over, at the phi's own width
    LowerValue* variable;  // the step, where it is a value rather than a number
    U64 step;
    bool widened;

    // §32.2 The no-wrap fact came from `checkedIndexCannotOverflow` rather than from
    // `stepCannotOverflow`, which is a weaker claim: the addition *may* wrap, and what is proved is
    // that nothing reads a pointer derived from a wrapped value. Enough to carry a pointer, not
    // enough to replace the counter - see the proof, which says which pass may believe it.
    bool checkedOnly;
};

// One address the loop recomputes, and everything needed to decide which pointer it becomes.
struct Candidate {
    LowerValue* address;  // the `add %base, %scaled` being replaced
    LowerValue* basePtr;
    LowerValue* initial;
    LowerValue* variable; // the step, where the loop adds a value rather than a number
    bool widened;         // whether the pointer's start has to sign-extend `initial` first
    U64 scale;
    U64 stride;           // what the pointer advances by, where the step is a number
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
 * The arm a bounds check on `%u` lets through, where `cmp` is that check.
 *
 * The shape is the one every subscript in the language emits: `cmp_ge %index, %length` - *unsigned*,
 * which is what makes one comparison answer both ends of the range - branching to a block control
 * never leaves. `Unreachable` is asked for by name rather than "a block outside the loop", because
 * what the proof above needs is that the failing arm does not go on to read anything.
 *
 * The bound has to have its top bit provably clear, and that is the whole of the arithmetic: a
 * negative index sign-extends to at least 0xffffffff80000000, so it compares at or above every bound
 * below 2^63 and the check rejects it. A container's length is a 32-bit field widened to 64 and is
 * far inside that; an arbitrary `Size` is not, and is declined.
 */
LowerBlock* boundsCheckPassArm(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::Cmp) return nullptr;

    auto cmp = (LowerInstCmp*)inst;
    if(cmp->getCmp() != LowerCmp::ge) return nullptr;

    auto bound = base[cmp->rhs];
    if(!isWide(bound->type)) return nullptr;
    if(!(knownZeroBits(base, bound) & (U64(1) << 63))) return nullptr;

    // The comparison has to be the branch's whole condition, and the branch has to be the terminator
    // of the block the comparison is in - anything else is a value this cannot follow to an arm.
    auto block = base[inst->block];
    if(!block->terminator || base[block->terminator]->kind != LowerInst::Je) return nullptr;

    auto je = (LowerInstJe*)base[block->terminator];
    if(base[je->cond] != cmp->created().ptr) return nullptr;

    auto fail = base[je->then];
    if(!fail->terminator || base[fail->terminator]->kind != LowerInst::Unreachable) return nullptr;

    return base[je->otherwise];
}

/*
 * §32.2 The third no-wrap proof, and the only one that lets the wrap happen.
 *
 * The two above establish that the narrow addition cannot overflow. This one establishes something
 * weaker and, for the *address* rewrite, sufficient: that if it does overflow, nothing ever reads a
 * pointer computed from the overflowed value. `Sieve`'s marking loop is what it is for -
 *
 *     while m < limit:  flags[m] = 0;  m = m + p
 *
 * where `m` is an `Int`, the address it drives is a `Ptr`, and the step is the prime being sieved.
 * Neither existing proof reaches it: the `for`-range guard is not emitted for a `while`, and the
 * strict-test argument needs the step to be exactly one.
 *
 * ## The argument
 *
 * The subscript's own bounds check is the evidence, and it is still standing here:
 *
 *     %w = sext %m       %u = bitcast %w       %c = cmp_ge %u, %L      je %c, abort, body
 *
 * **Reaching the body proves `%m >= 0`.** A negative `%m` sign-extends to at least
 * 0xffffffff80000000, so `%u >=u %L` for any `%L` below that - and `%L` is a container's length,
 * which arrives here as a 32-bit field widened to 64, so its top thirty-three bits are provably
 * clear. That is the step §32.2 recorded as "not airtight" and it is airtight after all: the bound
 * is not an arbitrary `Size`, it is one `knownZeroBits` can see the width of.
 *
 * **An overflow of `%m ± %p` from a non-negative `%m` always produces a negative result**, whatever
 * the sign of the step and with no fact about it needed. Adding: the sum exceeds the signed maximum
 * only if the step was positive, and wrapping past it sets the sign bit. Subtracting: the same, with
 * the signs mirrored. Underflow cannot happen at all - `%m >= 0` puts the true result at or above
 * the type's minimum in both directions.
 *
 * **So a wrap is rejected before it is read.** The next iteration enters the header with a negative
 * counter, reaches the same check, and aborts - and the pointer the reduction carries, which by then
 * disagrees with `base + sext(%m) * scale`, is never dereferenced. The unreduced program aborts at
 * exactly the same point, so the two are the same program.
 *
 * ## Where it may and may not be used
 *
 * **The address rewrite only.** `widenLoopCounters` replaces the counter itself, and there the
 * wrapped value is *observable*: a narrow counter that wraps negative re-enters the loop and aborts,
 * where a widened one keeps counting up, fails `%m < limit` and leaves normally. Two different
 * programs. `removeCheckedBounds` is worse than unsound, it is circular - it deletes the very check
 * this reads. So the fact is carried on `Induction::checkedOnly` rather than folded into
 * `stepCannotOverflow`, and both of those decline it.
 *
 * The conditions below are what make "before it is read" true rather than likely: the failing arm
 * must be a block control never leaves, the passing arm must dominate the step, and every use of the
 * widened index outside the check itself must be dominated by the passing arm.
 */
bool checkedIndexCannotOverflow(LowerBase base, const LoopInfo& loops, const ReducibleLoop& loop,
                                const DominatorTree& dominators, LowerInstPhi* phi, LowerInst* stepInst)
{
    // The step has to run inside the loop, or the counter reaching the check is not the one it
    // advances. Every other use of the counter is asked about through the widening below.
    if(!loops.contains(loop.header->index, base[stepInst->block]->index)) return false;

    auto checked = false;

    for(auto u: phi->result.uses.contents(base)) {
        auto widening = base[u];
        if(widening->kind != LowerInst::Cast || widening->createdCount != 1) continue;

        auto conversion = (LowerInstCast*)widening;
        if(!conversion->isSignedSource() || !conversion->isSignedResult()) continue;
        if(!isWide(conversion->result.type) || !isInt(conversion->result.type)) continue;

        auto index = &conversion->result;

        /*
         * The check this widening feeds. Found from the index's own readers rather than by walking
         * the loop: a bounds check is a comparison of the index, and the `bitcast` between them is
         * the `Size`/`I64` renaming every subscript goes through - the same alias walk
         * `removeCheckedBounds` performs one function down.
         */
        LowerBlock* pass = nullptr;

        auto findCheck = [&](LowerValue* alias) {
            for(auto c: alias->uses.contents(base)) {
                if(auto found = boundsCheckPassArm(base, base[c])) pass = found;
            }
        };

        findCheck(index);

        for(auto c: index->uses.contents(base)) {
            auto inst = base[c];

            // Through the bitcast, which is the same bits under the other name - a subscript
            // compares its index as a `Size` where the counter is an `I64`.
            if(inst->kind == LowerInst::Bitcast && inst->createdCount == 1 &&
               isWide(inst->created().ptr->type))
            {
                findCheck(inst->created().ptr);
            }
        }

        if(!pass) return false;
        if(!pass->dominates(base[stepInst->block], dominators)) return false;

        /*
         * And nothing reads the widened index anywhere the check has not run. The comparison and the
         * bitcast in front of it are the check itself and are the two readers that may sit above the
         * passing arm; anything else is an address this pass is about to make a carried pointer, and
         * one read on a path that skipped the check would be read after a wrap.
         */
        for(auto r: index->uses.contents(base)) {
            auto reader = base[r];
            if(reader->kind == LowerInst::Cmp || reader->kind == LowerInst::Bitcast) continue;
            if(!pass->dominates(base[reader->block], dominators)) return false;
        }

        checked = true;
    }

    return checked;
}

/*
 * Whether a header phi counts by the same amount every iteration.
 *
 * The value it takes on the latch edge has to be `%phi + C`, or `%phi + %s` for an `%s` the loop does
 * not compute. Nothing has to be checked about *where* that addition sits: it is not a phi and it is
 * live on the latch's outgoing edge, so its block dominates the latch and it therefore runs exactly
 * once per iteration - which is the whole of what "advanced by a fixed amount" needs.
 *
 * A counter narrower than the address unit is admitted only where `stepCannotOverflow` proves the
 * narrow addition cannot wrap, since that is exactly what makes `sext(%i + C) == sext(%i) + C` - and
 * so what makes the widened sequence an induction variable rather than a different sequence that
 * usually agrees. That proof reads the stride as a number, so a *narrow* counter with a runtime step
 * never gets past it - which is also the reason a variable step needs no width case of its own.
 *
 * `sub %phi, %s` is declined for a runtime `%s` while `sub %phi, C` is taken. The difference is only
 * that a negative amount is spelled by the constant itself in one case and would need a negation
 * emitted in the preheader in the other, and nothing this compiler emits counts down by a value.
 */
Maybe<Induction> inductionOf(LowerBase base, const LoopInfo& loops, const ReducibleLoop& loop,
                             const DominatorTree& dominators, LowerInstPhi* phi)
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

    // `add C, %i` as well as `add %i, C`: the operand canonicalization that settles which side a
    // literal is on belongs to the x64 backend, and has not run.
    if(lhs == &phi->result) {
        stride = rhs;
    } else if(rhs == &phi->result && inst->kind == LowerInst::Add) {
        stride = lhs;
    } else {
        return Nothing();
    }

    if(auto constant = immediateOf(base, stride)) {
        // Sign-extended out of its own width before anything multiplies it: a `-1` stored at `Int32`
        // is `0xffffffff`, and a stride of four billion is not a stride of minus one.
        auto signedStep = signedValue(constant.unwrap(), bits);
        step = inst->kind == LowerInst::Sub ? U64(0) - U64(signedStep) : U64(signedStep);

        if(step == 0) return Nothing();

        auto checkedOnly = false;

        if(narrow && !stepCannotOverflow(base, loops, loop, phi, inst, stride,
                                         inst->kind == LowerInst::Add))
        {
            // §32.2 And the check the subscript already performs, which proves less and is enough
            // for the pointer. `checkedOnly` is what keeps that distinction downstream.
            if(!checkedIndexCannotOverflow(base, loops, loop, dominators, phi, inst)) return Nothing();
            checkedOnly = true;
        }

        return Just(Induction { initial, nullptr, step, narrow, checkedOnly });
    }

    // §32 A step the loop does not compute, and the case §32.2 left unreachable: a *narrow* counter
    // stepped by a value has no proof that reads the stride as a number, so the only one available
    // is the one that does not read it at all. `Sieve`'s marking loop is exactly this shape.
    if(inst->kind != LowerInst::Add) return Nothing();
    if(stride->type != phi->result.type) return Nothing();
    if(!outsideLoop(base, loops, loop.header->index, stride)) return Nothing();

    if(narrow && !checkedIndexCannotOverflow(base, loops, loop, dominators, phi, inst)) {
        return Nothing();
    }

    return Just(Induction { initial, stride, 0, narrow, narrow });
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

    /*
     * What the pointer advances by. A step that is a number is an immediate in the latch, where the
     * addition is; a step that is a value is that value times the scale, multiplied out in the
     * *preheader* - it is the same product on every iteration, and putting it in the latch would be
     * a multiply per iteration in exchange for the shift this pass is removing.
     *
     * §32.2 A *narrow* step is sign-extended first, exactly as the counter's start is one line up and
     * for the same reason: what the pointer advances by is `sext(%p) * scale`, and the no-wrap fact
     * is what says that is the same number as `sext(%i + %p) - sext(%i)`. Only reachable since the
     * third proof - before it a variable step implied a wide counter, and the widening had nothing
     * to do. Left out, the multiply is an `Int32` against an `Int64` immediate and `validateFunction`
     * reports it, which is how it was found.
     */
    auto step = shape.variable && shape.widened
        ? resultOf(cast<true, true>(base, module, *loop.pre, shape.variable, LowerType::Int64, StringId()))
        : shape.variable;

    auto stride = shape.variable
        ? resultOf(binary<LowerInst::Mul>(base, module, *loop.pre, step,
                                          immediate(base, module, *loop.pre, LowerType::Int64,
                                                    shape.scale),
                                          LowerType::Int64, StringId()))
        : immediate(base, module, *loop.latch, LowerType::Int64, shape.stride);

    auto advanced = binary<LowerInst::Add>(base, module, *loop.latch, &phi->result, stride,
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
        if(candidate.variable != shape.variable) continue;
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

        into.push(Candidate { address, pointer, counter.initial, counter.variable, counter.widened,
                              scale, counter.step * scale });
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
        auto induction = inductionOf(base, loops, loop, dominators, phi);
        if(!induction) continue;

        auto& counter = induction.unwrap();

        // The counter's start has to reach the preheader, where the pointer's start is computed. It
        // does by construction - it is the alternative the preheader edge carries - but the check
        // states it, since a value defined inside the loop is one the setup cannot read.
        if(!outsideLoop(base, loops, header, counter.initial)) continue;

        // §32 And so does a step that is a value, which is multiplied out there. Being outside the
        // loop implies it wherever the graph is reducible, and is asked outright for the reason
        // `collectScaledAddresses` asks it of the base: that is exactly what LoopInfo does not
        // promise.
        if(counter.variable &&
           !base[counter.variable->inst()->block]->dominates(loop.pre, dominators))
        {
            continue;
        }

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
               done.variable == candidate.variable && done.widened == candidate.widened &&
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
                       const DominatorTree& dominators, const ReducibleLoop& loop, DeadSweep& sweep)
{
    auto& arena = fun.arena;
    auto header = loop.header->index;

    SmallArray<LowerInstPhi*, 8> phis;
    for(auto p: loop.header->phis.contents(base)) phis.push(base[p]);

    for(auto phi: phis) {
        auto induction = inductionOf(base, loops, loop, dominators, phi);
        if(!induction) continue;

        auto& found = induction.unwrap();
        if(!found.widened) continue;   // already at the address unit's width
        if(found.variable) continue;   // and so is every counter stepped by a value - see Induction

        /*
         * §32.2 And every counter whose no-wrap fact is the bounds check rather than the arithmetic.
         *
         * That proof permits the wrap and shows nothing *reads* what it produced; here the wrapped
         * value is the thing being replaced, and a counter that wraps negative re-enters the loop
         * and aborts where a widened one leaves it normally. Two different programs, so the pointer
         * rewrite may believe it and this may not.
         */
        if(found.checkedOnly) continue;
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

/*
 * §28 A bounds check the loop's own test has already made - see the header.
 */

// A value read past the bitcasts standing in front of it, where each of them keeps every bit: the
// lowering writes one wherever a `Size` meets an `I64` or a pointer meets an integer, so a length
// and the counter compared against it usually arrive here through one each. Bounded rather than
// followed to the end, for the reason `allocationBase` in lower_forward.cpp gives.
LowerValue* throughBitcasts(LowerBase base, LowerValue* value) {
    for(auto steps = 0; steps < 4; steps++) {
        auto inst = value->inst();
        if(inst->kind != LowerInst::Bitcast) return value;

        auto from = base[((LowerInstUnary*)inst)->from];
        if(!isIntLike(from->type) || !isIntLike(value->type)) return value;
        if(widthOf(from->type) != widthOf(value->type)) return value;

        value = from;
    }

    return value;
}

/*
 * §28 Whether the loop's bound is at or below the length the check reads, as unsigned patterns.
 *
 * Two shapes, and the second is the one every subscript of a borrowed array has. `length(xs) :: Int`
 * truncates a `Size` to 32 bits and the counter is then widened back, so the bound the header
 * compares against is `sext(trunc(%L))` while the check compares against `%L` itself:
 *
 *  - `%L mod 2^32 <=u %L` for every unsigned `%L`, since taking the low half of a number never
 *    raises it;
 *  - and `sext` either reproduces that value, when the truncation's top bit is clear, or produces a
 *    negative one - which the header's *signed* test then fails against a counter that starts at
 *    zero, so the body never runs and there is nothing to have proved.
 *
 * The truncation is required to be a real narrowing and the extension a real widening; a cast
 * between two 64-bit types is not this argument, and neither is one whose source is signed.
 */
bool boundWithinLength(LowerBase base, LowerValue* bound, LowerValue* length) {
    bound = throughBitcasts(base, bound);
    length = throughBitcasts(base, length);

    if(bound == length) return true;

    auto widen = bound->inst();
    if(widen->kind != LowerInst::Cast) return false;

    auto extend = (LowerInstCast*)widen;
    if(!extend->isSignedSource() || !extend->isSignedResult()) return false;

    auto narrow = throughBitcasts(base, base[extend->from]);
    if(widthOf(narrow->type) >= widthOf(bound->type)) return false;

    auto truncate = narrow->inst();
    if(truncate->kind != LowerInst::Cast) return false;

    // A truncation and nothing else. `isSignedSource` would make it a sign-preserving narrowing,
    // which is a different value for exactly the patterns this is about.
    auto cut = (LowerInstCast*)truncate;
    if(cut->isSignedSource() || cut->isSignedResult()) return false;

    auto source = throughBitcasts(base, base[cut->from]);
    if(widthOf(source->type) <= widthOf(narrow->type)) return false;

    return source == length;
}

// The header's own test, where it is one that bounds `%i` from above on the arm that stays inside
// the loop. Null for a header that branches on anything else, or that keeps both arms in the loop -
// in which case a block it dominates is not one the test decided.
LowerValue* loopUpperBound(LowerBase base, const LoopInfo& loops, const ReducibleLoop& loop,
                           LowerInstPhi* phi)
{
    auto headerJe = base[loop.header->terminator];
    if(headerJe->kind != LowerInst::Je) return nullptr;

    auto branch = (LowerInstJe*)headerJe;
    if(!loops.contains(loop.header->index, base[branch->then]->index)) return nullptr;
    if(loops.contains(loop.header->index, base[branch->otherwise]->index)) return nullptr;

    auto test = comparisonOf(base, base[branch->cond]);
    auto bound = comparedAgainst(base, test, &phi->result);
    if(!bound || bound == &phi->result) return nullptr;

    auto kind = test->getCmp();
    if(kind != LowerCmp::ilt && kind != LowerCmp::ile) return nullptr;

    return bound;
}

/*
 * §28 One counter of one loop, and every check inside it the counter's own bounds answer.
 *
 * The counter has to start non-negative and ascend without wrapping, which together with the header
 * test gives `0 <= %i <= %B` in every block the header dominates. `stepCannotOverflow` is the
 * no-wrap half and is the same one the two passes above use.
 */
void removeCheckedBounds(LowerBase base, LowerModule& module, LowerFunction& fun, const LoopInfo& loops,
                         const DominatorTree& dominators, const ReducibleLoop& loop,
                         SmallArray<LowerBlock*, 8>& dropped)
{
    for(auto phiPtr: loop.header->phis.contents(base)) {
        auto phi = base[phiPtr];
        auto counter = &phi->result;

        auto initial = phiValueFrom(base, phi, loop.pre);
        auto stepped = phiValueFrom(base, phi, loop.latch);
        if(!initial || !stepped) continue;

        auto start = immediateOf(base, initial);
        if(!start || I64(signedValue(start.unwrap(), widthOf(counter->type))) < 0) continue;

        auto stepInst = stepped->inst();
        if(stepInst->kind != LowerInst::Add) continue;

        auto binary = (LowerInstBinary*)stepInst;
        if(base[binary->lhs] != counter) continue;
        if(!stepCannotOverflow(base, loops, loop, phi, stepInst, base[binary->rhs], true)) continue;

        auto bound = loopUpperBound(base, loops, loop, phi);
        if(!bound) continue;

        /*
         * Every comparison that names the counter, which is where a check names it. The walk is over
         * the uses rather than over the loop's blocks because a check is a use of the index and
         * there are a handful of those against however many instructions the body has.
         *
         * The bitcasts are walked with it. A check compares the index as a `Size` and the counter is
         * an `I64`, so what the comparison names is a bitcast of the phi and never the phi - and a
         * bitcast between two 64-bit integers is the same bits under another name, which is what
         * makes its result the counter for every purpose here.
         */
        SmallArray<LowerValue*, 4> aliases;
        aliases.push(counter);

        for(Size i = 0; i < aliases.size(); i++) {
            for(auto aliasPtr: aliases[i]->uses.contents(base)) {
                auto user = base[aliasPtr];
                if(user->kind != LowerInst::Bitcast) continue;

                auto produced = user->created().ptr;
                if(throughBitcasts(base, produced) != counter) continue;
                if(!aliases.containsValue(produced)) aliases.push(produced);
            }
        }

        // The comparison itself is left where it is, with nothing reading it; `removeDeadValues` at
        // the end of the pipeline takes it and the bitcast in front of it. That is also what makes
        // continuing this walk safe - what the rewrite changes is the *branch's* use of the
        // comparison, and no list being iterated here is one of those.
        for(Size i = 0; i < aliases.size(); i++) {
        for(auto userPtr: aliases[i]->uses.contents(base)) {
            auto compare = base[userPtr];
            if(compare->kind != LowerInst::Cmp) continue;

            auto cmp = (LowerInstCmp*)compare;
            if(throughBitcasts(base, base[cmp->lhs]) != counter) continue;

            auto kind = cmp->getCmp();
            if(kind != LowerCmp::ge && kind != LowerCmp::lt) continue;  // unsigned, against a length
            if(!boundWithinLength(base, bound, base[cmp->rhs])) continue;

            // The one reader has to be the branch ending the block the comparison is in, and that
            // block has to be one the header's decision covers.
            if(cmp->result.uses.size() != 1) continue;

            auto block = base[compare->block];
            if(!loops.contains(loop.header->index, block->index)) continue;
            if(!headerDominates(loop, dominators, block)) continue;
            if(!block->terminator || base[block->terminator]->kind != LowerInst::Je) continue;

            auto je = (LowerInstJe*)base[block->terminator];
            if(base[je->cond] != &cmp->result) continue;

            // `%i >=u %L` aborts on the taken arm and `%i <u %L` on the other, so which arm has to
            // be unreachable follows the comparison. Both are refused unless that arm actually is
            // one: an arm this cannot see the end of is a live path, and the check stays.
            auto abortArm = kind == LowerCmp::ge ? base[je->then] : base[je->otherwise];
            auto keptArm = kind == LowerCmp::ge ? base[je->otherwise] : base[je->then];
            if(abortArm == keptArm) continue;
            if(!abortArm->terminator || base[abortArm->terminator]->kind != LowerInst::Unreachable) continue;
            if(abortArm->phis.isNotEmpty()) continue;

            // The rewrite: the block stops branching and the abort arm loses its one edge from here.
            detach(base, (LowerInst*)je);

            auto jmp = (LowerInst*)new (module.arena) LowerInstJmp(keptArm - base);
            jmp->block = block - base;
            block->terminator = jmp - base;
            block->outgoing[0] = keptArm - base;
            block->outgoing[1] = nullptr;

            for(Size i = 0; i < abortArm->incoming.size(); i++) {
                if(abortArm->incoming.get(base, i) != block - base) continue;

                abortArm->incoming.remove(base, i);
                break;
            }

            if(abortArm->incoming.isEmpty() && !dropped.containsValue(abortArm)) dropped.push(abortArm);
        }
        }
    }
}

// Taking a block nothing reaches any more out of the function - the same routine lower_merge.cpp
// keeps, and needed here for the same reason: the x64 block ordering asserts that no such block
// exists rather than skipping one. Its instructions stop reading what they read, which is what
// leaves the sweeps below something to collect.
void dropUnreachedBlock(LowerBase base, LowerFunction& fun, LowerBlock* block) {
    for(auto instPtr: block->instructions.contents(base)) detach(base, base[instPtr]);
    if(block->terminator) detach(base, base[block->terminator]);

    for(Size i = 0; i < fun.blocks.size(); i++) {
        if(fun.blocks.get(base, i) != block - base) continue;

        fun.blocks.remove(base, i);
        break;
    }

    for(Size i = 0; i < fun.blocks.size(); i++) base[fun.blocks.get(base, i)]->index = BlockIndex(i);
}

} // namespace

void eliminateBoundedChecks(LowerBase base, LowerModule& module, LowerFunction& fun,
                            const LoopAnalysis& analysis)
{
    if(fun.blocks.size() < 2) return;

    auto& loops = analysis.loops;
    auto& dominators = analysis.dominators;

    // Collected rather than removed in place: dropping a block renumbers every one of them, which is
    // what `loops` and the dominator tree are indexed by.
    SmallArray<LowerBlock*, 8> dropped;

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(!loops.isHeader(block->index)) continue;

        if(auto loop = reducibleLoop(base, loops, dominators, block)) {
            removeCheckedBounds(base, module, fun, loops, dominators, loop.unwrap(), dropped);
        }
    }

    for(auto block: dropped) dropUnreachedBlock(base, fun, block);
}

void reduceInductionVariables(LowerBase base, LowerModule& module, LowerFunction& fun,
                              const LoopAnalysis& analysis)
{
    if(fun.blocks.size() < 2) return;

    // Both valid for the whole walk: nothing below creates, removes or renumbers a block, so the loop
    // structure and the dominator tree the caller found are the ones the last line reduces against -
    // which is also what keeps `postIndex`, the tree's index, standing. See LoopAnalysis for why the
    // two passes behind this one are handed the same pair rather than rebuilding it.
    auto& loops = analysis.loops;
    auto& dominators = analysis.dominators;
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

void widenInductionVariables(LowerBase base, LowerModule& module, LowerFunction& fun,
                             const LoopAnalysis& analysis)
{
    if(fun.blocks.size() < 2) return;

    // The same two answers the reduction above read, and valid for the same reason: nothing there,
    // nothing in the fold between the two, and nothing here creates, removes or renumbers a block.
    auto& loops = analysis.loops;
    auto& dominators = analysis.dominators;
    DeadSweep sweep { base, module, {} };

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(!loops.isHeader(block->index)) continue;

        if(auto loop = reducibleLoop(base, loops, dominators, block)) {
            widenLoopCounters(base, module, fun, loops, dominators, loop.unwrap(), sweep);
        }
    }

    sweep.sweep(fun);
}
