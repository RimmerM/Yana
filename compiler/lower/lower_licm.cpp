#include "lower_licm.h"
#include "lower_builder.h"

namespace {

// How far an address is followed back through constant offsets. Field addressing is one `add` and
// the occasional pair of them, so a budget rather than a loop guard is enough - and it keeps this
// question at a fixed cost per candidate.
static const U32 kOffsetDepth = 4;

// An address as a base value and a constant displacement from it. `add %p, 8` is the only shape the
// lowering builds for a field, so peeling it is the whole of the normalization: everything else -
// an index, a scaled offset, a pointer that came out of memory - is its own base at offset zero.
struct Address {
    LowerValue* value;
    U64 offset;
};

Address addressOf(LowerBase base, LowerValue* value) {
    Address result { value, 0 };

    for(U32 i = 0; i < kOffsetDepth; i++) {
        auto inst = result.value->inst();
        if(inst->kind != LowerInst::Add) break;

        auto binary = (LowerInstBinary*)inst;
        auto rhs = base[binary->rhs];
        if(rhs->inst()->kind != LowerInst::Imm || !isInt(rhs->type)) break;

        result.offset += ((LowerImm*)rhs->inst())->i;
        result.value = base[binary->lhs];
    }

    return result;
}

// Whether an instruction may write storage a hoisted read could see. Asked of the whole loop rather
// than of a pair of addresses - see lower_licm.h for why the crude answer is the one taken here.
bool writesStorage(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Store:
        case LowerInst::Copy:
        case LowerInst::SetPattern:
        case LowerInst::Call:
        case LowerInst::Intrinsic:
            return true;
        default:
            return false;
    }
}

/*
 * Whether a computation may fault where it is being moved to.
 *
 * One bit on one kind, and it is *not* the old "a division can trap" rule this replaces. Every
 * division `makeDivisionTotal` leaves is total and hoists freely - that is most of what defining
 * `x / 0` bought. The exception is the division that pass left unguarded because a test above it had
 * already settled the divisor: it cannot fault where it stands and would above the test, so it is
 * the one computation whose safety is a property of its *position* rather than of its operands.
 * See LowerInstBinary::kTrustsDivisorTest, which is set nowhere else.
 *
 * Nothing else in `isRepeatable` can fault: a shift count is masked, a float operation answers a
 * NaN, and a vector operation is lane-wise arithmetic.
 */
bool mayFault(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Div: case LowerInst::IDiv:
        case LowerInst::Rem: case LowerInst::IRem:
            return ((LowerInstBinary*)inst)->trustsDivisorTest();
        default:
            return false;
    }
}

/*
 * Whether moving this computation buys anything, which is a narrower question than whether it may be
 * moved - and the one that decides whether this pass is worth having. Measured rather than reasoned:
 * hoisting everything invariant costs **Matrix +1.4 ms and Sort +5.2 ms** against hoisting the two
 * kinds below.
 *
 * **An address is free where it stands.** `add %xs, 8` reaching a load is a displacement in that
 * load's addressing mode - `foldAddresses` in codegen/x64/transform.cpp puts it there, and the SIB
 * byte holds a scale beside it - so the loop does not execute it at all. Hoisting one trades a byte
 * of encoding for a register live across the whole loop, and `Sort.yana`'s partition loop is five
 * `add %xs, 8` and two `add %v_1, 12` of exactly that shape. It is the same gate `lower_induction.h`
 * states for the scale of a reduced address, arrived at from the other side: an addressing mode the
 * machine encodes for nothing is not a computation the loop repeats.
 *
 * A pointer result is the whole of the test, and it is deliberately a *type* question rather than a
 * search over the users. An address is what a pointer-typed value is in this IR - the lowering builds
 * one per field access and per element - and the one place a pointer arrives without being an address
 * is a call's return value, which is not repeatable and never reaches here.
 *
 * **A comparison is free where it stands too, and for the same kind of reason.** One whose readers
 * are branches is carried in the flags - `canCarryInFlags` in codegen/x64/transform.cpp - so the loop
 * spends a `cmp` and a `jcc` and no register at all. Moved to the preheader it is out of every flags
 * window there is, which makes it a `setcc` into a register, that register live across the loop, and
 * a `test` in front of the branch that used to read the flags directly. That is one instruction more
 * in the loop rather than one fewer.
 *
 * **A `Set` is not a computation.** It is a copy the register allocator's coalescing removes where it
 * can, so hoisting one moves a copy rather than removing it.
 */
bool worthHoisting(LowerInst* inst) {
    if(inst->kind == LowerInst::Set || inst->kind == LowerInst::Cmp) return false;
    return ((LowerInstSingle*)inst)->result.type != LowerType::Pointer;
}

// How far an instruction reaches past its own base address, for the two kinds that touch memory.
// Zero for everything else, which is what makes the search below a filter rather than a switch.
//
// An overreading load answers zero, which takes it out of the search entirely - see the comment in
// `safeToSpeculate` on why it may not vouch for anything. It is not a load whose *own* extent is
// unknown; it is one whose extent is deliberately past the object, and both readers here would take
// it for a statement about how large the object is.
U64 accessExtent(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Load:  return ((LowerInstLoad*)inst)->isOverread() ? 0 : ((LowerInstLoad*)inst)->getWidth();
        case LowerInst::Store: return ((LowerInstStore*)inst)->getWidth();
        default:               return 0;
    }
}

LowerValue* accessAddress(LowerBase base, LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Load:  return base[((LowerInstLoad*)inst)->from];
        case LowerInst::Store: return base[((LowerInstStore*)inst)->to];
        default:               return nullptr;
    }
}

/*
 * Whether the bytes this load reads are storage that exists whether or not the loop body runs.
 *
 * Two answers, both about the address rather than about the loop - see lower_licm.h. The second is
 * stated as "the object extends at least this far from its base", which is what a dominating access
 * at a further offset says: an access at `%p + 12` of four bytes is a statement that `%p` names
 * sixteen bytes, and `%p + 0` of eight is inside them.
 *
 * An overreading load says the opposite of that and is excluded from both sides of the search by
 * `accessExtent` answering zero for one. Its whole content is that it reads past the object on
 * purpose, so reading it as a claim about the object's size is the one way this rule can be wrong.
 */
bool safeToSpeculate(LowerBase base, LowerFunction& fun, const DominatorTree& dominators,
                     LowerBlock* preheader, const Address& address, U64 extent) {
    auto inst = address.value->inst();

    if(inst->kind == LowerInst::Alloca) {
        auto size = base[((LowerInstAlloca*)inst)->byteCount];
        if(size->inst()->kind != LowerInst::Imm) return false;

        return address.offset + extent <= ((LowerImm*)size->inst())->i;
    }

    // The accesses that reach this base, at whatever constant offset of their own, from a block the
    // preheader is reached through. A record read at one field vouches for every field below it.
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(block != preheader && !block->dominates(preheader, dominators)) continue;

        for(auto instPtr: block->instructions.contents(base)) {
            auto user = base[instPtr];

            auto width = accessExtent(user);
            if(!width) continue;

            auto at = accessAddress(base, user);
            if(!at) continue;

            auto other = addressOf(base, at);
            if(other.value != address.value) continue;

            if(other.offset + width >= address.offset + extent) return true;
        }
    }

    return false;
}

// The one predecessor of a loop header from outside it, where that block exists and does nothing but
// enter the loop. An instruction put there runs exactly when the loop is entered, which is half of
// what makes moving a read into it sound - the other half is `safeToSpeculate`.
LowerBlock* preheaderOf(LowerBase base, const LoopInfo& loops, LowerBlock* header) {
    if(header->incoming.size() != 2) return nullptr;

    auto first = base[header->incoming.get(base, 0)];
    auto second = base[header->incoming.get(base, 1)];

    auto firstInside = loops.contains(header->index, first->index);
    if(firstInside == loops.contains(header->index, second->index)) return nullptr;

    auto pre = firstInside ? second : first;
    if(!pre->terminator || base[pre->terminator]->kind != LowerInst::Jmp) return nullptr;

    return pre;
}

/*
 * Whether this loop contains no loop of its own.
 *
 * The gate on hoisting a *computation*, and the second of the two things measurement decided rather
 * than reasoning - see `worthHoisting` for the first. `Matrix.multiply` is a three-deep nest with an
 * invariant multiply at two of the three levels, and hoisting them is worth **-0.9%** for the inner
 * one and **+1.4%** for both:
 *
 *  - `k * n` sits in the innermost loop and leaves it. That takes an `imul` out of the block the
 *    program spends its time in - 19 instructions to 18 - and it is §50.2's measurement, reproduced.
 *  - `row * n` sits one level out, in the `k` loop, and is already read from inside the innermost
 *    one. Hoisting it removes nothing from the innermost loop; what it changes is that the product
 *    now has to survive the `k` loop's back edge as well, and the `k` loop is where this function's
 *    pressure already is - `row` itself is spilled to make room, and the whole of the inner loop's
 *    gain is handed back.
 *
 * That is the general shape rather than this program's accident. What a hoist buys is one instruction
 * per iteration of the loop it leaves; what it costs is a value live across every iteration of every
 * loop *inside* that one. Leaving an innermost loop is the case where the second term is empty, and
 * it is also the case where the first term is largest, because the innermost loop is the hot one.
 *
 * Multi-level hoisting falls out of this rather than being forbidden by it - just not in one step. A
 * value leaving an innermost loop lands in that loop's preheader, and if the enclosing loop then has
 * no other loop in it, the next round takes it out of that one too. What cannot happen is a value
 * being carried across a loop it was not already inside.
 *
 * A load is not held to this. Its rule is `safeToSpeculate` and its saving is a memory access rather
 * than an arithmetic instruction, which is a different trade and one already measured (§10 item 1).
 */
bool isInnermost(LowerBase base, LowerFunction& fun, const LoopInfo& loops, BlockIndex header) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(block->index == header) continue;
        if(loops.contains(header, block->index) && loops.isHeader(block->index)) return false;
    }

    return true;
}

// Whether a value is defined outside the loop, which is the whole of "it does not change between
// iterations" for a value in SSA form. An argument belongs to no block and is outside every loop
// there is.
bool definedOutside(LowerBase base, const LoopInfo& loops, BlockIndex header, LowerValue* value) {
    auto definition = value->inst()->block;
    return !definition || !loops.contains(header, base[definition]->index);
}

/*
 * Whether every operand of `inst` is one the preheader can read, collecting the immediates that have
 * to travel with it.
 *
 * An `Imm` inside the loop is admitted and pushed onto `constants`: it has no operands of its own, so
 * the only thing standing between it and the preheader is that nobody moved it. Recorded rather than
 * moved on sight because the caller may still decline the instruction for another operand, and a
 * constant relocated for a hoist that did not happen is a `mov` bought for nothing.
 */
bool operandsAvailable(LowerBase base, const LoopInfo& loops, BlockIndex header, LowerInst* inst,
                       SmallArray<LowerInst*, 8>& constants) {
    auto used = inst->used();

    for(Size i = 0; i < used.length; i++) {
        auto value = base[used.ptr[i]];
        if(definedOutside(base, loops, header, value)) continue;

        auto definition = value->inst();
        if(definition->kind != LowerInst::Imm) return false;

        constants.push(definition);
    }

    return true;
}

/*
 * Whether a load's address is the same every iteration, collecting the arithmetic that has to travel
 * with it - §58.4 of test/bench/findings.md.
 *
 * `definedOutside` alone is what this used to ask, and it refuses the shape the corpus's loads are
 * actually in. `Sort`'s partition scan reads the container's length through `%v = add %xs, 8` and the
 * `add` sits *in the loop*, because that is where the subscript that needed it was written. The
 * address is invariant - `%xs` is a parameter and the offset is a literal - but the load's operand is
 * a value defined inside, so the load was declined and the length reloaded on every iteration. The
 * x64 load fold then folds it into the compare's memory operand, which is why this costs a memory
 * access per iteration rather than an instruction and why reading the disassembly does not show it.
 *
 * The arithmetic is not hoisted on its own account and must not be: `worthHoisting` refuses an
 * address deliberately, because a displacement is free where it stands and hoisting one buys a live
 * range and sells nothing. What changes here is that it is no longer on its own account - it is the
 * price of taking the load out, and the load is a memory access per iteration. So it travels with
 * its reader exactly as an immediate does, and for the same reason: it is recorded rather than moved
 * on sight, since the caller may still decline the load on `safeToSpeculate` and an address
 * relocated for a hoist that did not happen is the `lea` this pass exists not to buy.
 *
 * Only address arithmetic is followed, and only where every leaf is invariant. That is `Add` and the
 * `Imm` it reads - an indexed address has an operand the loop advances and fails at the leaf, which
 * is the same refusal `definedOutside` gave and the reason no element access reaches this.
 *
 * The walk pushes an instruction *after* its own operands, so the caller moving the list in order
 * puts every definition in front of its reader.
 *
 * **It is worth no measurable time and no bytes, and is kept anyway.** Over the sixteen benchmark
 * programs it moves one byte, and over the 186 `test/resolve` executables it moves none; `Sort` is
 * the only row in either whose code changes, and its scans are short and their operand is in L1. It
 * stays because the pass's own statement of what it is for - "a read whose address does not change
 * between iterations is one read, done once" - was not true of the commonest address the lowering
 * emits, which is a gap rather than a trade.
 */
bool invariantAddress(LowerBase base, const LoopInfo& loops, BlockIndex header, LowerValue* value,
                      SmallArray<LowerInst*, 8>& carried, Size depth = 0)
{
    if(definedOutside(base, loops, header, value)) return true;

    // Address arithmetic is a short chain - a base and a constant offset, sometimes twice. The limit
    // is what keeps a cyclic or pathological one from walking, since a phi is not admitted anyway.
    if(depth >= 4) return false;

    auto inst = value->inst();
    if(inst->kind != LowerInst::Add && inst->kind != LowerInst::Imm) return false;

    auto used = inst->used();
    for(Size i = 0; i < used.length; i++) {
        if(!invariantAddress(base, loops, header, base[used.ptr[i]], carried, depth + 1)) return false;
    }

    // Already carried by another operand of the same load, which an address reading one immediate
    // twice would produce. Moving it twice is harmless and listing it twice is not worth the risk.
    for(auto seen: carried) if(seen == inst) return true;

    carried.push(inst);
    return true;
}

/*
 * Moving one instruction to the end of the preheader's instruction list, which is in front of that
 * block's terminator: a terminator is not in the instruction list. The instruction keeps its operands
 * and its result, so every reader of it stays pointed at the same value - and the preheader dominates
 * the whole loop, so every one of them is still dominated by the definition.
 *
 * Through `detach` first, because `addInst` is what *records* an instruction's uses: adding one that
 * is already in its operands' use lists would put it there twice, and the validator counts both
 * directions.
 *
 * The block it came from still names it. `inst->block` is the preheader once this returns, and that
 * is what the sweep at the end of each loop filters the loop's instruction lists on - which is what
 * lets a constant be taken out of a block the walk has already passed.
 */
void moveToPreheader(LowerBase base, LowerBlock* preheader, LowerInst* inst) {
    if(inst->block == preheader - base) return;

    detach(base, inst);
    inst->block = nullptr;
    preheader->addInst(base, inst);
}

} // namespace

void hoistLoopInvariants(LowerBase base, LowerModule& module, LowerFunction& fun,
                         const LoopAnalysis& analysis)
{
    if(fun.blocks.size() < 2) return;

    // The caller's, and valid for the whole walk: everything below moves instructions between
    // blocks that already exist, so neither the loop structure nor the dominance relation moves
    // with them. See LoopAnalysis.
    auto& loops = analysis.loops;
    auto& dominators = analysis.dominators;

    /*
     * Innermost first, and repeated until nothing moves. A value leaving an inner loop lands in that
     * loop's preheader, which is a block the enclosing loop contains - so carrying it the rest of
     * the way out is another round rather than another rule, and the walk below is in no particular
     * order to begin with. That is also what orders the preheader: an operand hoisted this round is
     * outside the loop next round, and its reader is appended behind it.
     */
    auto changed = true;
    while(changed) {
        changed = false;

        for(auto headerPtr: fun.blocks.contents(base)) {
            auto header = base[headerPtr];
            if(!loops.isHeader(header->index)) continue;

            auto preheader = preheaderOf(base, loops, header);
            if(!preheader) continue;

            // What the loop does to storage, once per loop rather than once per candidate. Only the
            // loads are held to it; a computation reads nothing that a store could change.
            auto writes = false;
            for(auto blockPtr: fun.blocks.contents(base)) {
                auto block = base[blockPtr];
                if(!loops.contains(header->index, block->index)) continue;

                for(auto instPtr: block->instructions.contents(base)) {
                    if(writesStorage(base[instPtr])) { writes = true; break; }
                }

                if(writes) break;
            }

            // And whether a computation leaving it would have to be carried across a loop of its
            // own, which is the gate the arithmetic is held to and the loads are not. Once per loop,
            // for the reason above.
            auto innermost = isInnermost(base, fun, loops, header->index);
            auto moved = false;

            for(auto blockPtr: fun.blocks.contents(base)) {
                auto block = base[blockPtr];
                if(!loops.contains(header->index, block->index)) continue;

                for(auto instPtr: block->instructions.contents(base)) {
                    auto inst = base[instPtr];

                    // A constant moved out from under this walk by an earlier candidate. Its own
                    // block still lists it and the sweep below is what takes it off; until then it
                    // is simply not here any more.
                    if(inst->block != blockPtr) continue;

                    // Inline: the immediates one candidate would have to take with it, which is at
                    // most one per operand.
                    SmallArray<LowerInst*, 8> constants;

                    if(inst->kind == LowerInst::Load) {
                        if(writes) continue;

                        auto load = (LowerInstLoad*)inst;
                        auto from = base[load->from];

                        // The address has to be the same one every iteration - and where it is
                        // computed inside the loop out of things that are, the computation comes
                        // out with the load. See `invariantAddress`.
                        if(!invariantAddress(base, loops, header->index, from, constants)) continue;

                        auto address = addressOf(base, from);
                        if(!safeToSpeculate(base, fun, dominators, preheader, address, load->getWidth())) {
                            continue;
                        }
                    } else if(innermost && isRepeatable(inst) && !mayFault(inst) && worthHoisting(inst)) {
                        if(!operandsAvailable(base, loops, header->index, inst, constants)) continue;
                    } else {
                        continue;
                    }

                    // The constants first, so that the preheader defines them in front of the reader
                    // that needed them moved.
                    for(auto constant: constants) moveToPreheader(base, preheader, constant);
                    moveToPreheader(base, preheader, inst);
                    moved = true;
                }
            }

            if(!moved) continue;

            // What is left of each block in the loop, which is everything that still says it is
            // there. Done once per loop rather than once per block, because a constant leaves a
            // block the walk above may already have finished with.
            for(auto blockPtr: fun.blocks.contents(base)) {
                auto block = base[blockPtr];
                if(!loops.contains(header->index, block->index)) continue;

                // Inline: one of these per block, holding the instructions that stay while the list
                // it came from is rebuilt - the same shape lower_fold.cpp and lower_cse.cpp use.
                SmallArray<LowerPtr<LowerInst>, 32> kept;
                for(auto instPtr: block->instructions.contents(base)) {
                    if(base[instPtr]->block == blockPtr) kept.push(instPtr);
                }

                if(kept.size() == block->instructions.size()) continue;

                block->instructions.clear();
                for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
            }

            changed = true;
        }
    }
}
