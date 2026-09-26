#include "lower_licm.h"
#include "lower_builder.h"
#include "lower_fold.h"
#include "lower_promote.h"
#include "lower_store.h"

namespace {

/*
 * Whether moving this computation buys anything, which is a narrower question than whether it may be
 * moved - and the one that decides whether this pass is worth having. Measured rather than reasoned:
 * hoisting everything invariant costs **Matrix +1.4 ms and Sort +5.2 ms** against hoisting the two
 * kinds below.
 *
 * **An address is free where it stands.** `add %xs, 8` reaching a load is a displacement in that
 * load's addressing mode - `foldAddresses` in codegen/x64/transform_address.cpp puts it there, and the SIB
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
 * are branches is carried in the flags - `canCarryInFlags` in codegen/x64/transform_peephole.cpp - so the loop
 * spends a `cmp` and a `jcc` and no register at all. Moved to the preheader it is out of every flags
 * window there is, which makes it a `setcc` into a register, that register live across the loop, and
 * a `test` in front of the branch that used to read the flags directly. That is one instruction more
 * in the loop rather than one fewer.
 *
 * **A `Set` is not a computation.** It is a copy the register allocator's coalescing removes where it
 * can, so hoisting one moves a copy rather than removing it.
 */
bool worthHoisting(LowerInst* inst) {
    if(inst->kind == LowerInst::Load && ((LowerInstLoad*)inst)->isVolatile()) return false;
    if(inst->kind == LowerInst::Set || inst->kind == LowerInst::Cmp) return false;
    return ((LowerInstSingle*)inst)->result.type != LowerType::Pointer;
}

/*
 * How far an access vouches past its own base address, which is not quite how far it reaches.
 *
 * The extent is the instruction's - see `LowerInst::accessAt` - and what is here is the one thing
 * this pass does not take at face value. An overreading load answers zero, which takes it out of the
 * search entirely: it is not a load whose *own* extent is unknown, it is one whose extent is
 * deliberately past the object, and a reader here would take it for a statement about how large the
 * object is. A copy or a fill answers zero for the ordinary reason - its length is an operand, and
 * this pass has no constant for it.
 */
U64 vouchedExtent(LowerInst* inst, const LowerAccess& access) {
    if(inst->kind == LowerInst::Load && ((LowerInstLoad*)inst)->isOverread()) return 0;
    return access.bytes;
}

/*
 * Whether the bytes this load reads are storage that exists whether or not the loop body runs.
 *
 * Two answers, both about the address rather than about the loop - see lower_licm.h. The second is
 * stated as "the object extends at least this far from its base", which is what a dominating access
 * at a further offset says: an access at `%p + 12` of four bytes is a statement that `%p` names
 * sixteen bytes, and `%p + 0` of eight is inside them. A write vouches as readily as a read, and an
 * atomic as readily as a plain one: what makes the statement is that the bytes were touched.
 *
 * An overreading load says the opposite of that and is excluded from both sides of the search by
 * `vouchedExtent` answering zero for one. Its whole content is that it reads past the object on
 * purpose, so reading it as a claim about the object's size is the one way this rule can be wrong.
 *
 * **Both sides are placed by `placeOf`** (lower_store.h), and that is J163's fix rather than a
 * tidying. This pass used to walk addresses with a copy of its own that took any `add` immediate as
 * an offset, so `add %p, -16` was offset 2^64-16 and an eight-byte access there "vouched" that
 * `%p + 0 .. 8` existed - the sum wrapped. A program reading `*(p - 2)` in front of a loop that
 * reads `*p` and runs zero times then faulted on the preheader's load at a page boundary, which is
 * review r5/p3's `spec3` for the hoister and `spec4` for `promoteLoopCells`, both in
 * `test/resolve/SpeculatedLoad.yana`. `placeOf` refuses an offset past 2^32 - a negative one, or
 * one about to wrap - so such an access has no place and vouches for nothing, and the queried
 * address likewise has no place and is declined. The comparisons below are written so that they
 * cannot wrap either, whatever a caller's extent is.
 */
bool safeToSpeculate(LowerBase base, LowerFunction& fun, const DominatorTree& dominators,
                     LowerBlock* preheader, HashMap<U32, U8>& contained, const LowerPlace& place,
                     U64 extent) {
    if(!place.root || extent == 0 || extent == kUnbounded) return false;

    if(place.allocation) {
        auto size = base[((LowerInstAlloca*)place.root->inst())->byteCount];
        if(size->inst()->kind != LowerInst::Imm) return false;

        auto bytes = ((LowerImm*)size->inst())->i;
        return extent <= bytes && place.offset <= bytes - extent;
    }

    // The accesses that reach this base, at whatever constant offset of their own, from a block the
    // preheader is reached through. A record read at one field vouches for every field below it.
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(block != preheader && !block->dominates(preheader, dominators)) continue;

        for(auto instPtr: block->instructions.contents(base)) {
            auto user = base[instPtr];

            LowerAccess accesses[kMaxAccesses];
            auto count = lowerInstAccesses(user, accesses);

            for(Size which = 0; which < count; which++) {
                auto width = vouchedExtent(user, accesses[which]);
                if(!width) continue;

                auto other = placeOf(base, contained, *accesses[which].address, width);
                if(other.root != place.root) continue;

                // `other.offset + width >= place.offset + extent`, which cannot wrap: both offsets
                // are at most 2^32 by `placeOf`, and each width is an access's own.
                if(other.offset + width >= place.offset && other.offset + width - place.offset >= extent) {
                    return true;
                }
            }
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


/*
 * What a loop does to storage, gathered once per loop - J158.
 *
 * The question a load is held to used to be "does anything in the loop write at all", and that
 * refused every loop that stores anything: an element store anywhere in the body kept every bound
 * and every field read of an unrelated slot inside it. The places each write lands are what
 * `forwardStoredValues` already names for a block (`LowerPlace` in lower_store.h), and the aliasing rule
 * it applies to them is the one this asks of a whole loop: two allocations are two objects, a
 * contained allocation is reached by nothing but its own address, and one root is compared by range.
 *
 * Three things are kept whole rather than placed:
 *
 *  - **a write that names no address** - a call, an intrinsic - which may write anything except a
 *    contained allocation, since nothing handed it that address;
 *  - **anything ordered** - an atomic, a fence, a spin hint - and **anything volatile**, which end
 *    every question. A read moved across an acquire is a read from before it, and a load hoisted
 *    out of a polling loop is the rewrite that turns a spin into a hang (kLowerOrdered).
 */
struct LoopWrites {
    // Inline: a loop body's writes, which are few - an element store and a counter or two.
    SmallArray<LowerPlace, 16> places;
    SmallArray<LowerInst*, 16> writers;
    bool opaque = false;
    bool ordered = false;
    bool any = false;
};

void gatherLoopWrites(LowerBase base, LowerFunction& fun, const LoopInfo& loops, BlockIndex header,
                      HashMap<U32, U8>& contained, LoopWrites& into) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        if(!loops.contains(header, block->index)) continue;

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            if(hasLowerTrait(inst, kLowerOrdered) || isVolatileMemoryAccess(inst)) into.ordered = true;
            if(!hasLowerTrait(inst, kLowerWrites)) continue;
            into.any = true;

            LowerAccess accesses[kMaxAccesses];
            auto count = lowerInstAccesses(inst, accesses);

            if(count == 0) {
                into.opaque = true;
                continue;
            }

            // A copy writes its first access and reads its second; every other kind naming an
            // address is taken to write each one it names.
            if(inst->kind == LowerInst::Copy) count = 1;

            for(Size a = 0; a < count; a++) {
                into.places.push(placeOf(base, contained, *accesses[a].address, placeExtent(base, accesses[a])));
                into.writers.push(inst);
            }
        }
    }
}

// Whether anything the loop does may write a byte of `place` - other than the writes `except` says
// are its own, which is how the promotion below asks it about a cell whose stores it keeps.
template<class Except>
bool loopMayWrite(const LoopWrites& writes, const LowerPlace& place, Except&& except) {
    if(writes.ordered) return true;
    if(writes.opaque && !place.contained) return true;

    for(Size i = 0; i < writes.places.size(); i++) {
        if(except(writes.writers[i])) continue;
        if(mayOverlap(writes.places[i], place)) return true;
    }

    return false;
}

bool loopMayWrite(const LoopWrites& writes, const LowerPlace& place) {
    return loopMayWrite(writes, place, [](LowerInst*) { return false; });
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

    // The allocations nothing but their own accesses reach, once per function: nothing here adds a
    // use of an address that is not a load of it, so the answer holds for the whole walk.
    HashMap<U32, U8> contained;
    collectContainedAllocations(base, fun, contained);

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

            // What the loop does to storage, once per loop rather than once per candidate - and where,
            // which is what J158 changed: a loop that writes is no longer a loop none of whose reads
            // may leave, only one whose reads of what it writes may not. See `LoopWrites`.
            LoopWrites writes;
            gatherLoopWrites(base, fun, loops, header->index, contained, writes);

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
                        auto load = (LowerInstLoad*)inst;
                        auto from = base[load->from];

                        if(writes.any || writes.ordered) {
                            if(load->isVolatile()) continue;

                            // An overreading load reads bytes it does not name, which its place has
                            // to include - the same widening `forwardStoredValues` applies.
                            auto extent = load->isOverread() ? kUnbounded : U64(load->getWidth());
                            if(loopMayWrite(writes, placeOf(base, contained, load->from, extent))) continue;
                        }

                        // The address has to be the same one every iteration - and where it is
                        // computed inside the loop out of things that are, the computation comes
                        // out with the load. See `invariantAddress`.
                        if(!invariantAddress(base, loops, header->index, from, constants)) continue;

                        auto width = U64(load->getWidth());
                        auto place = placeOf(base, contained, load->from, width);
                        if(!safeToSpeculate(base, fun, dominators, preheader, contained, place, width)) {
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

namespace {

/*
 * One location a loop both reads and writes, as `promoteLoopCells` finds it - see lower_licm.h.
 *
 * The place is a root defined outside the loop and a constant offset from it, and every load of it
 * in the loop reads it at one width, into one type, with one extension: two loads that agree on all
 * three answer the same value from the same bytes, and a store that writes the load's type at that
 * width answers it too (`storedAnswer`). A cell whose loads disagree is not one this carries.
 */
struct LoopCell {
    LowerPlace place;
    LowerType type = LowerType::Int64;
    U32 width = 0;
    bool isSigned = false;
    bool loaded = false;
    bool stored = false;
    bool refused = false;
};

// The value a load of the cell reads back after this store, or null where it is not the stored value
// itself - the rule `forwardedValue` in lower_store.cpp states for one block, without the reshaping:
// a value this pass would have to build is one it declines to carry round a loop.
LowerValue* storedAnswer(LowerBase base, LowerInstStore* store, const LoopCell& cell) {
    if(store->isVolatile() || store->getWidth() != cell.width) return nullptr;

    auto stored = base[store->value];
    if(stored->type != cell.type) return nullptr;

    auto accessed = cell.width * 8;
    if(accessed >= registerBits(cell.type)) return stored;
    if(!isInt(cell.type) || cell.isSigned) return nullptr;

    auto bits = registerBits(cell.type);
    auto above = (bits >= 64 ? ~U64(0) : (U64(1) << bits) - 1) & ~((U64(1) << accessed) - 1);
    return (knownZeroBits(base, stored) & above) == above ? stored : nullptr;
}

bool samePlace(const LowerPlace& a, const LowerPlace& b) {
    return a.root && a.root == b.root && a.offset == b.offset && a.bytes == b.bytes;
}

/*
 * The rewrite for one cell of one loop, which is SSA construction over the cell with its stores kept.
 *
 * What the cell holds at the top of a block is found by walking up the loop: a block with one
 * predecessor holds what that predecessor ended with, the header holds a phi over the preheader's
 * read and the latch's end, and any other join holds a phi of its own, made when first asked and
 * filled from a worklist - so a loop of any depth is a walk rather than a recursion. What a block
 * ends with is its last store's value, or what it started with. A load is then whatever the cell
 * held just above it, and goes.
 *
 * Built unattached and attached only once every question has been answered, because the one failure
 * a walk can meet - a block of the loop with no predecessor, which only an unreachable block is - has
 * to leave the function as it was. A phi that turns out to merge one value is that value.
 */
struct CellRewrite {
    LowerBase base;
    LowerModule& module;
    LowerFunction& fun;
    const LoopInfo& loops;
    LowerBlock* header;
    LowerBlock* preheader;
    HashMap<U32, U8>& contained;
    const LoopCell& cell;

    // Indexed by block index, sized to the function.
    SmallArray<LowerValue*, 64> lastStore;
    SmallArray<LowerInstPhi*, 64> joinPhi;
    SmallArray<LowerBlock*, 16> pending;
    SmallArray<LowerInstPhi*, 16> phis;
    SmallArray<LowerBlock*, 16> phiBlocks;

    // What each replaced load and each merged-away phi stands for - resolved through `resolve`.
    HashMap<U32, LowerPtr<LowerValue>> standsFor;
    SmallArray<LowerInstLoad*, 16> loads;

    // Where the stores go when they leave the loop - only for a contained cell, see `planSinking`.
    bool sink = false;
    SmallArray<LowerBlock*, 4> sinkInto;
    SmallArray<LowerValue*, 4> sinkValues;
    SmallArray<LowerInstStore*, 8> stores;

    LowerInstPhi* headerPhi = nullptr;
    LowerInstLoad* entry = nullptr;
    LowerInst* entryAddress = nullptr;
    LowerInst* entryOffset = nullptr;
    bool failed = false;

    bool inCell(LowerInst* inst, bool& isStore) {
        if(inst->kind == LowerInst::Load) {
            auto load = (LowerInstLoad*)inst;
            if(load->isVolatile() || load->isOverread()) return false;
            isStore = false;
            return samePlace(placeOf(base, contained, load->from, load->getWidth()), cell.place);
        }

        if(inst->kind == LowerInst::Store) {
            auto store = (LowerInstStore*)inst;
            if(store->isVolatile()) return false;
            isStore = true;
            return samePlace(placeOf(base, contained, store->to, store->getWidth()), cell.place);
        }

        return false;
    }

    LowerInstPhi* newPhi(LowerBlock* block) {
        auto phi = makePhi(module.arena, cell.type, U32(block->incoming.size()));
        phis.push(phi);
        phiBlocks.push(block);
        return phi;
    }

    // What the cell holds on entry to `block`.
    LowerValue* startOf(LowerBlock* block) {
        for(Size steps = 0; steps <= fun.blocks.size(); steps++) {
            if(block == header) return &headerPhi->result;
            if(auto phi = joinPhi[block->index]) return &phi->result;

            if(block->incoming.size() == 0) break;

            if(block->incoming.size() == 1) {
                auto pred = base[block->incoming.get(base, 0)];
                if(!loops.contains(header->index, pred->index)) break;
                if(auto value = lastStore[pred->index]) return value;

                block = pred;
                continue;
            }

            auto phi = newPhi(block);
            joinPhi[block->index] = phi;
            pending.push(block);
            return &phi->result;
        }

        failed = true;
        return nullptr;
    }

    LowerValue* endOf(LowerBlock* block) {
        if(auto value = lastStore[block->index]) return value;
        return startOf(block);
    }

    LowerPtr<LowerValue> resolve(LowerPtr<LowerValue> value) {
        for(;;) {
            auto next = standsFor.getValue(U32(value));
            if(next.isNothing()) return value;
            value = next.unwrap();
        }
    }

    /*
     * The stores of a **contained** cell, moved to the edges the loop leaves by - the other half of
     * the classic promotion, and only where it needs no argument about anyone else.
     *
     * A contained allocation is reached by nothing but its own address (`addressContained`), so no
     * call and no other thread can read it: the loop's own loads were the only readers inside, and
     * they are gone. What is left to honour is the reads *after* the loop, which see the cell as the
     * last store left it - so each exit gets one store of what the cell holds on that edge. Leaving
     * before the first store writes back what was read on entry, which for storage nobody else can
     * see is no write at all.
     *
     * Each exit has to be one this can put a store on without splitting the edge: a block entered
     * from this loop block and nothing else. A block with no successors that does not touch the cell
     * needs no store at all - the frame the cell lives in ends there, whether by a `ret` or by an
     * abort - which is what a bounds check's shared abort arm is. Any other exit keeps the stores.
     * So does a read of the cell in the loop that is not one of its loads - a copy out of it, a
     * narrower load of one of its bytes - since that reader needs memory to be current.
     */
    bool planSinking() {
        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];
            if(!loops.contains(header->index, block->index)) continue;

            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];
                auto isStore = false;
                if(inCell(inst, isStore)) continue;
                if(!hasLowerTrait(inst, kLowerReads)) continue;

                LowerAccess accesses[kMaxAccesses];
                auto count = lowerInstAccesses(inst, accesses);
                for(Size a = 0; a < count; a++) {
                    auto place = placeOf(base, contained, *accesses[a].address, placeExtent(base, accesses[a]));
                    if(mayOverlap(place, cell.place)) return false;
                }
            }

            for(auto successorPtr: block->successors(base)) {
                auto exit = base[successorPtr];
                if(loops.contains(header->index, exit->index)) continue;

                if(!exit->hasSuccessors() && !touchesRoot(exit)) continue;

                auto seen = false;
                for(auto done: sinkInto) if(done == exit) seen = true;
                if(seen) continue;

                // Every way into the exit is from this loop - a `break` and the test failing both
                // arrive at the block after the loop - so what the cell holds there is a merge of
                // what each of those blocks ended with, and nothing else reaches the block to need
                // a value the loop does not have.
                for(auto pred: exit->incoming.contents(base)) {
                    if(!loops.contains(header->index, base[pred]->index)) return false;
                }

                LowerValue* value;
                if(exit->incoming.size() == 1) {
                    value = endOf(block);
                } else {
                    auto phi = newPhi(exit);
                    for(Size i = 0; i < exit->incoming.size(); i++) {
                        auto pred = base[exit->incoming.get(base, i)];
                        auto end = endOf(pred);
                        if(failed) return false;

                        phi->used().ptr[i] = end - base;
                        phi->sources()[i] = pred - base;
                    }

                    value = &phi->result;
                }

                if(failed) return false;

                sinkInto.push(exit);
                sinkValues.push(value);
            }
        }

        return true;
    }

    // Whether anything in a block names an address under the cell's root.
    bool touchesRoot(LowerBlock* block) {
        for(auto instPtr: block->instructions.contents(base)) {
            LowerAccess accesses[kMaxAccesses];
            auto count = lowerInstAccesses(base[instPtr], accesses);

            for(Size a = 0; a < count; a++) {
                auto place = placeOf(base, contained, *accesses[a].address, placeExtent(base, accesses[a]));
                if(!place.root || place.root == cell.place.root) return true;
            }
        }

        return false;
    }

    bool plan() {
        Size blockCount = 0;
        for(auto blockPtr: fun.blocks.contents(base)) blockCount = max(blockCount, Size(base[blockPtr]->index) + 1);

        for(Size i = 0; i < blockCount; i++) { lastStore.push(nullptr); joinPhi.push(nullptr); }

        // Every block of the loop but the header is entered from inside it, which is what a natural
        // loop is - asked rather than assumed, since the walk below is only sound where it holds.
        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];
            if(block == header || !loops.contains(header->index, block->index)) continue;

            for(auto pred: block->incoming.contents(base)) {
                if(!loops.contains(header->index, base[pred]->index)) return false;
            }

            for(auto instPtr: block->instructions.contents(base)) {
                auto isStore = false;
                if(!inCell(base[instPtr], isStore) || !isStore) continue;

                lastStore[block->index] = storedAnswer(base, (LowerInstStore*)base[instPtr], cell);
                if(!lastStore[block->index]) return false;
            }
        }

        for(auto instPtr: header->instructions.contents(base)) {
            auto isStore = false;
            if(!inCell(base[instPtr], isStore) || !isStore) continue;

            lastStore[header->index] = storedAnswer(base, (LowerInstStore*)base[instPtr], cell);
            if(!lastStore[header->index]) return false;
        }

        // The read on entry: the root, the offset in front of it where there is one, and the load.
        LowerValue* address = cell.place.root;
        if(cell.place.offset) {
            entryOffset = new (module.arena) LowerImm(StringId(), LowerType::Int64, cell.place.offset);
            entryAddress = new (module.arena) LowerInstBinary(StringId(), LowerType::Pointer, cell.place.root - base,
                                                              &((LowerImm*)entryOffset)->result - base, LowerInst::Add);
            address = &((LowerInstBinary*)entryAddress)->result;
        }

        entry = new (module.arena) LowerInstLoad(address - base, StringId(), cell.type, cell.width, cell.isSigned);
        headerPhi = newPhi(header);

        // Every load of the cell, answered by what the cell held just above it.
        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];
            if(!loops.contains(header->index, block->index)) continue;

            LowerValue* current = nullptr;
            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];
                auto isStore = false;
                if(!inCell(inst, isStore)) continue;

                if(isStore) {
                    current = storedAnswer(base, (LowerInstStore*)inst, cell);
                    stores.push((LowerInstStore*)inst);
                    continue;
                }

                auto value = current ? current : startOf(block);
                if(failed) return false;

                auto load = (LowerInstLoad*)inst;
                standsFor.add(U32(&load->result - base), value - base);
                loads.push(load);
            }
        }

        if(failed) return false;
        if(sink && !planSinking()) sink = false;
        if(failed) return false;

        // The header's alternatives, and then every join the walks above asked about.
        for(Size i = 0; i < header->incoming.size(); i++) {
            auto pred = base[header->incoming.get(base, i)];
            auto value = pred == preheader ? &entry->result : endOf(pred);
            if(failed) return false;

            headerPhi->used().ptr[i] = value - base;
            headerPhi->sources()[i] = pred - base;
        }

        while(pending.size()) {
            auto block = pending.pop().unwrap();
            auto phi = joinPhi[block->index];

            for(Size i = 0; i < block->incoming.size(); i++) {
                auto pred = base[block->incoming.get(base, i)];
                auto value = endOf(pred);
                if(failed) return false;

                phi->used().ptr[i] = value - base;
                phi->sources()[i] = pred - base;
            }
        }

        // A phi merging one value, or one value and itself, is that value.
        for(auto changed = true; changed;) {
            changed = false;

            for(auto phi: phis) {
                auto self = &phi->result - base;
                if(standsFor.getValue(U32(self)).isJust()) continue;

                LowerPtr<LowerValue> only = nullptr;
                auto several = false;

                for(auto operand: phi->used()) {
                    auto value = resolve(operand);
                    if(value == self || value == only) continue;
                    if(only) { several = true; break; }
                    only = value;
                }

                if(several || !only) continue;

                standsFor.add(U32(self), only);
                changed = true;
            }
        }

        // A phi left merging nothing but itself is a cycle no edge from the header enters, which
        // only blocks nothing reaches form. Declined rather than given a value it cannot have.
        for(auto phi: phis) {
            auto self = &phi->result - base;
            if(standsFor.getValue(U32(self)).isJust()) continue;

            auto grounded = false;
            for(auto operand: phi->used()) if(resolve(operand) != self) { grounded = true; break; }
            if(!grounded) return false;
        }

        /*
         * Worth it only where the merges are not more than the loads they replace - J160.
         *
         * A cell read once in a loop that is a state machine - an iterator object's step inlined
         * into its consumer - reaches that read through every join the machine has, and each join
         * is a phi: J158 found `iter/Object` trading 18 loads for 58 phis and `NestedItemObject` 16
         * for 164. A phi the allocator cannot give one register on every edge is a move per edge,
         * and a register held across the whole loop for one read is pressure on everything else in
         * it. The header's own phi is the one the promotion exists to make, so it is not counted
         * against the loads; every other surviving merge is.
         */
        Size merges = 0;
        for(auto phi: phis) {
            if(phi == headerPhi || standsFor.getValue(U32(&phi->result - base)).isJust()) continue;
            merges++;
        }

        if(merges > loads.size()) return false;

        return true;
    }

    void apply() {
        if(entryOffset) preheader->addInst(base, entryOffset);
        if(entryAddress) preheader->addInst(base, entryAddress);
        preheader->addInst(base, entry);

        for(Size i = 0; i < phis.size(); i++) {
            auto phi = phis[i];
            if(standsFor.getValue(U32(&phi->result - base)).isJust()) continue;

            // Resolved before the phi is attached, since attaching it is what registers its reads.
            auto used = phi->used();
            for(Size k = 0; k < used.length; k++) used.ptr[k] = resolve(used.ptr[k]);
            phiBlocks[i]->addInst(base, phi);
        }

        if(sink) {
            auto address = base[entry->from];

            for(Size i = 0; i < sinkInto.size(); i++) {
                auto exit = sinkInto[i];
                auto value = base[resolve(sinkValues[i] - base)];
                auto store = new (module.arena) LowerInstStore(address - base, value - base, cell.width);
                exit->addInst(base, store);

                // To the front: the exit's own instructions may read the cell.
                for(auto k = exit->instructions.size() - 1; k > 0; k--) {
                    exit->instructions.set(base, k, exit->instructions.get(base, k - 1));
                }
                exit->instructions.set(base, 0, (LowerInst*)store - base);
            }

            for(auto store: stores) {
                detach(base, store);
                store->block = nullptr;
            }
        }

        // Merged-away phis were never attached, so nothing reads them once the loads' readers are
        // pointed past them.
        for(auto load: loads) {
            auto self = &load->result - base;
            replaceUses(base, module.arena, self, resolve(self));
            detach(base, load);
            load->block = nullptr;
        }
    }
};

// The loops of a function innermost first - by the number of blocks each holds, which orders a loop
// in front of every loop that contains it.
void loopsInnermostFirst(LowerBase base, LowerFunction& fun, const LoopInfo& loops,
                         SmallArray<LowerBlock*, 16>& into) {
    SmallArray<Size, 16> sizes;

    for(auto headerPtr: fun.blocks.contents(base)) {
        auto header = base[headerPtr];
        if(!loops.isHeader(header->index)) continue;

        Size size = 0;
        for(auto blockPtr: fun.blocks.contents(base)) {
            if(loops.contains(header->index, base[blockPtr]->index)) size++;
        }

        into.push(header);
        sizes.push(size);
    }

    for(Size i = 1; i < into.size(); i++) {
        for(Size j = i; j > 0 && sizes[j] < sizes[j - 1]; j--) {
            swap(into[j], into[j - 1]);
            swap(sizes[j], sizes[j - 1]);
        }
    }
}

} // namespace

CellPromotion promoteLoopCells(LowerBase base, LowerModule& module, LowerFunction& fun, const LoopAnalysis& analysis) {
    if(fun.blocks.size() < 2) return {};
    CellPromotion result;

    auto& loops = analysis.loops;
    auto& dominators = analysis.dominators;

    HashMap<U32, U8> contained;
    collectContainedAllocations(base, fun, contained);

    SmallArray<LowerBlock*, 16> headers;
    loopsInnermostFirst(base, fun, loops, headers);

    for(auto header: headers) {
        auto preheader = preheaderOf(base, loops, header);
        if(!preheader) continue;

        LoopWrites writes;
        gatherLoopWrites(base, fun, loops, header->index, contained, writes);
        if(writes.ordered || !writes.any) continue;

        // The cells: every plain load and store of the loop at a root defined outside it.
        SmallArray<LoopCell, 8> cells;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];
            if(!loops.contains(header->index, block->index)) continue;

            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];
                auto isLoad = inst->kind == LowerInst::Load;
                if(!isLoad && inst->kind != LowerInst::Store) continue;

                LowerPlace place;
                if(isLoad) {
                    auto load = (LowerInstLoad*)inst;
                    if(load->isVolatile() || load->isOverread()) continue;
                    place = placeOf(base, contained, load->from, load->getWidth());
                } else {
                    auto store = (LowerInstStore*)inst;
                    if(store->isVolatile()) continue;
                    place = placeOf(base, contained, store->to, store->getWidth());
                }

                if(!place.root || !definedOutside(base, loops, header->index, place.root)) continue;

                LoopCell* cell = nullptr;
                for(auto& c: cells) if(samePlace(c.place, place)) { cell = &c; break; }

                if(!cell) {
                    cells.push(LoopCell { .place = place });
                    cell = &cells[cells.size() - 1];
                }

                if(!isLoad) { cell->stored = true; continue; }

                auto load = (LowerInstLoad*)inst;
                if(!cell->loaded) {
                    cell->loaded = true;
                    cell->type = load->result.type;
                    cell->width = load->getWidth();
                    cell->isSigned = load->isSigned();
                } else if(cell->type != load->result.type || cell->width != load->getWidth() ||
                          cell->isSigned != load->isSigned()) {
                    cell->refused = true;
                }
            }
        }

        for(auto& cell: cells) {
            // A cell only read is the hoister's; one only written has nothing to answer.
            if(cell.refused || !cell.loaded || !cell.stored) continue;

            // Nothing else in the loop may write it: its own stores are the one exception, and they
            // are the stores of exactly this place.
            auto own = [&](LowerInst* writer) {
                if(writer->kind != LowerInst::Store) return false;
                auto store = (LowerInstStore*)writer;
                return !store->isVolatile() && samePlace(placeOf(base, contained, store->to, store->getWidth()), cell.place);
            };

            if(loopMayWrite(writes, cell.place, own)) continue;

            // The preheader reads it on a path the loop may not have read it on.
            if(!safeToSpeculate(base, fun, dominators, preheader, contained, cell.place, cell.width)) {
                continue;
            }

            CellRewrite rewrite { base, module, fun, loops, header, preheader, contained, cell };
            rewrite.sink = cell.place.contained;
            if(!rewrite.plan()) continue;

            rewrite.apply();
            result.promoted = true;
            if(rewrite.sink) result.sank = true;

            // What is left of each block, without the loads the rewrite answered.
            for(auto blockPtr: fun.blocks.contents(base)) {
                auto block = base[blockPtr];
                if(!loops.contains(header->index, block->index)) continue;

                SmallArray<LowerPtr<LowerInst>, 32> kept;
                for(auto instPtr: block->instructions.contents(base)) {
                    if(base[instPtr]->block == blockPtr) kept.push(instPtr);
                }

                if(kept.size() == block->instructions.size()) continue;

                block->instructions.clear();
                for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
            }
        }
    }
    return result;
}
