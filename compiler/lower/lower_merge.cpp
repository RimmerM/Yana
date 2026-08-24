#include "lower_merge.h"
#include "lower_builder.h"

/*
 * See lower_merge.h for what shape is recognized and why it is confined to blocks control never
 * leaves. This file is the comparison and the edge surgery.
 */

namespace {

/*
 * The largest block this will compare.
 *
 * Everything the pass is for is one to four instructions - a syscall and an `Unreachable`, a `mov`
 * and a `ret` - and the position lookup below is a linear scan, so the bound is what keeps the
 * comparison linear in the block rather than quadratic. A duplicated exit longer than this is not
 * something inlining produces: what gets copied is the *arm*, and an arm with a dozen instructions
 * in it is a function that was inlined into the arm rather than the arm itself.
 */
static constexpr Size kMaxExitSize = 16;

// Where a value is defined inside one candidate block, as `(instruction index << 8) | result index`,
// or `kNotLocal` for a value the block did not produce. A linear scan rather than a map: the blocks
// this is asked about are bounded by kMaxExitSize, and a map would allocate per comparison.
static constexpr U32 kNotLocal = maxLimit<U32>;

static U32 positionIn(LowerBase base, LowerBlock* block, LowerPtr<LowerValue> value) {
    auto insts = block->instructions.contents(base);

    for(U32 i = 0; i < insts.size(); i++) {
        auto created = base[insts[i]]->created();
        for(U32 j = 0; j < created.length; j++) {
            if(&created.ptr[j] - base == value) return (i << 8) | j;
        }
    }

    return kNotLocal;
}

/*
 * Whether two operands are the same value to a block that is about to be duplicated away.
 *
 * Three ways, in the order they occur. **The same position within the two blocks**, which is what
 * makes this a comparison of what the blocks *do* rather than of what their values are called: two
 * copies of one exit compute the same chain out of the same inputs and give every step of it a
 * different name.
 *
 * **The same value**, which is what an operand computed elsewhere and read by both copies is - the
 * condition an abort arm was branched on lives in the block above it, and both copies name that one.
 *
 * **Two immediates holding one number**, for the reason lower_cse.cpp states at greater length:
 * `immediate()` builds a fresh `Imm` per call, so two copies of `exit(134)` name two instructions
 * that are the same constant. The comparison is on the stored word, which for a floating one is its
 * bits, so `-0.0` never reads as `0.0`. A `Fun` and a `Global` are the same case one step out - both
 * are addresses the module names, both are built per reference, and both are what a call out of two
 * copied blocks reads.
 */
static bool sameExitOperand(LowerBase base, LowerBlock* left, LowerBlock* right,
                            LowerPtr<LowerValue> a, LowerPtr<LowerValue> b)
{
    if(!a || !b) return a == b;

    auto here = positionIn(base, left, a);
    auto there = positionIn(base, right, b);
    if(here != kNotLocal || there != kNotLocal) return here == there;

    if(a == b) return true;
    if(base[a]->type != base[b]->type) return false;

    auto x = base[a]->inst();
    auto y = base[b]->inst();
    if(x->kind != y->kind) return false;

    switch(x->kind) {
        case LowerInst::Imm:
            return ((LowerImm*)x)->i == ((LowerImm*)y)->i;
        case LowerInst::Fun:
            return ((LowerInstFun*)x)->target == ((LowerInstFun*)y)->target;
        case LowerInst::Global:
            return ((LowerInstGlobal*)x)->target == ((LowerInstGlobal*)y)->target;
        default:
            return false;
    }
}

/*
 * Whether everything an instruction carries beside its operands is compared by `kind`, `flags` and
 * the types of its results.
 *
 * An allow-list rather than a list of exclusions, because the failure mode of getting it wrong is a
 * miscompile and the failure mode of being too strict is a merge that does not happen. A kind added
 * to the IR later is refused here until somebody says what makes two of them the same.
 *
 * What `flags` covers is worth naming, since it is doing most of the work: a cast's two signedness
 * bits, a comparison's test, a load's or a store's width and signedness, a call's convention, a
 * block operation's chosen encoding. Every one of those is a way two instructions of one kind
 * produce different numbers out of the same operands.
 *
 * Three kinds carry something `flags` does not, and each of them is compared by that field outright:
 * an `Imm`'s stored word, and the target of a `Fun` or a `Global`. All three are usually reached
 * through `sameExitOperand` instead, since a constant lowered from the resolve IR is built in the
 * entry block rather than in an arm - but a `Fun` is *not*: the address of a called function is
 * materialized in the block that calls it, so an exit block whose whole content is a teardown call
 * holds one, and refusing the kind refused every such block.
 *
 * **`Alloca` is the refusal that is a decision rather than an omission.** Two copies of an exit that
 * each allocate are two distinct pieces of storage and only one of them ever runs, so unifying them
 * would be sound - but a frame slot is the one thing here whose identity outlives the block, and no
 * exit block allocates in any program this compiler emits.
 */
static bool sameCarriedData(LowerInst* inst, LowerInst* other) {
    switch(inst->kind) {
        case LowerInst::Imm:
            // The stored word, so a float is compared as its bits and `-0.0` never reads as `0.0`.
            return ((LowerImm*)inst)->i == ((LowerImm*)other)->i;
        case LowerInst::Fun:
            return ((LowerInstFun*)inst)->target == ((LowerInstFun*)other)->target;
        case LowerInst::Global:
            return ((LowerInstGlobal*)inst)->target == ((LowerInstGlobal*)other)->target;
        default:
            break;
    }

    switch(inst->kind) {
        case LowerInst::Nop:
        case LowerInst::Set:
        case LowerInst::Cast:
        case LowerInst::Bitcast:
        case LowerInst::Neg:
        case LowerInst::Not:
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::Div:
        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::Rol:
        case LowerInst::Ror:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
        case LowerInst::BitsUpTo:
        case LowerInst::GatherBits:
        case LowerInst::ScatterBits:
        case LowerInst::Crc32:
        case LowerInst::Cmp:
        case LowerInst::Select:

        // The byte reversal, which carries nothing beside its operand either.
        case LowerInst::Bswap:

        // Both floating-point kinds, which carry nothing beside their operands - so two of them in
        // two copies of a block are the same computation exactly when their operands are, which is
        // what the caller compares next.
        case LowerInst::Sqrt:
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:
        case LowerInst::Round:
        case LowerInst::Fma:

        // `ShaBinary` carries its instruction in `flags`, which the caller compares beside this -
        // which is exactly why the op lives there rather than past the operands. See the note on
        // `VecShuffle` below, which is the same question answered the other way.
        case LowerInst::ShaBinary:
        case LowerInst::Sha256Rounds:

        // Four of the five vector kinds. `VecShuffle` is deliberately not one of them: its pattern
        // is stored past its operands rather than in `flags`, so two of them that pick different
        // lanes out of the same pair of vectors would compare the same here.
        case LowerInst::VecSplat:
        case LowerInst::VecLane:
        case LowerInst::VecWithLane:
        case LowerInst::VecReduce:
        case LowerInst::Load:
        case LowerInst::Store:
        case LowerInst::Copy:
        case LowerInst::SetPattern:
        case LowerInst::Call:
        case LowerInst::Ret:
        case LowerInst::Unreachable:
            return true;
        default:
            return false;
    }
}

/*
 * One operand of one instruction, named by where it sits rather than by what it is.
 *
 * The same pair addresses the same slot in every copy of a block, which is what lets a disagreement
 * found by comparing two copies be read back out of a third. `instruction` indexes the block's
 * instruction list, or is its size for the terminator.
 */
struct Slot {
    U32 instruction;
    U32 operand;
};

static LowerInst* instructionAt(LowerBase base, LowerBlock* block, U32 index) {
    if(index >= block->instructions.size()) return base[block->terminator];
    return base[block->instructions.get(base, index)];
}

static bool holdsSlot(const SmallArray<Slot, 4>& slots, Slot slot) {
    for(auto& have: slots) {
        if(have.instruction == slot.instruction && have.operand == slot.operand) return true;
    }

    return false;
}

/*
 * Whether two operands that are *not* the same value may be carried by a phi in the block the copies
 * become, rather than refusing the merge outright. See lower_merge.h for the argument; this is the
 * two conditions it states.
 */
static bool carriableByPhi(LowerBase base, LowerBlock* left, LowerBlock* right, LowerInst* inst,
                           U32 operand, LowerPtr<LowerValue> a, LowerPtr<LowerValue> b)
{
    if(!a || !b) return false;
    if(base[a]->type != base[b]->type) return false;

    // Defined outside the copy on both sides, which is what makes the value available on the edge.
    // A value the block computes for itself is one the merge deletes.
    if(positionIn(base, left, a) != kNotLocal) return false;
    if(positionIn(base, right, b) != kNotLocal) return false;

    // The function a call calls, which includes a syscall's number. Merging two of these would turn
    // two direct calls into one indirect one.
    if(inst->kind == LowerInst::Call && operand == 0) return false;

    return true;
}

// Whether control can leave this block by any edge. The whole restriction the pass rests on - see
// lower_merge.h - asked off the terminator rather than off `outgoing`, since a terminator with no
// successors is what `Ret` and `Unreachable` are.
static bool isExit(LowerBase base, LowerBlock* block) {
    if(!block->terminator) return false;

    auto kind = base[block->terminator]->kind;
    return kind == LowerInst::Ret || kind == LowerInst::Unreachable;
}

/*
 * Whether one block may stand in for the other, and where the two disagree.
 *
 * `differences` is written only when the answer is yes, and it is the block's *own* disagreements
 * rather than a running set - the caller unions it, because the gate has to price the union before
 * committing to it.
 *
 * The belt at the end is the one thing here that is not a comparison. A block with no successors
 * dominates nothing, so no value it defines can be read outside it - that is the argument the whole
 * pass rests on, and it is an argument about the IR being in SSA form rather than something this
 * file established. Asking the use lists directly costs one walk of a two-instruction block and
 * turns a broken invariant somewhere else into a merge that does not happen.
 */
static bool interchangeable(LowerBase base, LowerBlock* a, LowerBlock* b,
                            SmallArray<Slot, 4>& differences)
{
    if(a->phis.size() || b->phis.size()) return false;
    if(a->instructions.size() != b->instructions.size()) return false;
    if(a->instructions.size() > kMaxExitSize) return false;

    SmallArray<Slot, 4> found;

    // One past the instruction list, which is the terminator - compared by the same rules as
    // everything in front of it, since a `ret` differing only in what it returns is the whole point.
    for(U32 i = 0; i <= a->instructions.size(); i++) {
        auto x = instructionAt(base, a, i);
        auto y = instructionAt(base, b, i);

        if(x->kind != y->kind) return false;
        if(x->flags != y->flags) return false;
        if(x->createdCount != y->createdCount || x->usedCount != y->usedCount) return false;
        if(!sameCarriedData(x, y)) return false;

        auto madeX = x->created();
        auto madeY = y->created();
        for(U32 j = 0; j < madeX.length; j++) {
            if(madeX.ptr[j].type != madeY.ptr[j].type) return false;
        }

        auto usedX = x->used();
        auto usedY = y->used();
        for(U32 j = 0; j < usedX.length; j++) {
            if(sameExitOperand(base, a, b, usedX.ptr[j], usedY.ptr[j])) continue;
            if(!carriableByPhi(base, a, b, x, j, usedX.ptr[j], usedY.ptr[j])) return false;

            found.push(Slot { i, j });
        }
    }

    for(auto instPtr: b->instructions.contents(base)) {
        for(auto& value: base[instPtr]->created()) {
            for(auto user: value.uses.contents(base)) {
                if(base[base[user]->block] != b) return false;
            }
        }
    }

    replaceContents(differences, found);
    return true;
}

/*
 * Whether a group of copies is worth collapsing, counted in instructions.
 *
 * What the merge removes is every copy but one, which is `size` instructions each. What it adds is
 * one phi per slot the copies disagree on, and a phi is a copy on every edge reaching the block - so
 * a group that disagrees in as many places as it has instructions pays for the merge with the merge.
 *
 * Two blocks that are nothing but `ret 0` and `ret 1` are the case this refuses, and it is the one
 * that matters: they would become one `ret`, two jumps and a phi, which is longer than what it
 * replaced. Four copies of `call reclaim, %xs ; ret <n>` are the case it takes - six instructions
 * removed against four copies added, and the four were already there as the materialization of the
 * constant each `ret` was about to hand back.
 *
 * A group that fails is declined whole rather than searched for a profitable subset. The identical
 * case cannot fail - no disagreement is no phi - so nothing that used to merge stops merging.
 */
static bool worthMerging(Size copies, Size size, Size differences, Size edges) {
    return (copies - 1) * size > differences * edges;
}

// Whether any one predecessor branches to both of these, which is the shape the edge rewrite below
// has no answer for: retargeting would leave a `je` whose two arms name one block, and turning that
// into a `jmp` is a second rewrite with a condition to sweep up behind it. Declined instead, since a
// conditional branch between two copies of one exit is not a shape anything emits.
static bool sharesAPredecessor(LowerBase base, LowerBlock* a, LowerBlock* b) {
    for(auto from: a->incoming.contents(base)) {
        for(auto other: b->incoming.contents(base)) {
            if(from == other) return true;
        }
    }

    return false;
}

// Every edge into `from`, pointed at `to` instead - the terminator's own target field, the source
// block's successor list and the destination's predecessor list, which are the three places an edge
// lives. `from` is left with no predecessors, which is what makes it removable.
static void redirectEdges(LowerBase base, Region<LowerRegion>& arena, LowerBlock* from, LowerBlock* to) {
    // The sources, deduplicated. A `je` naming one block twice is listed twice in `incoming` and the
    // rewrite below moves both arms at once, so visiting such a source a second time would rewrite
    // nothing and record an edge that is not there.
    SmallArray<LowerPtr<LowerBlock>, 8> sources;

    for(auto source: from->incoming.contents(base)) {
        auto seen = false;
        for(auto& already: sources) {
            if(already == source) { seen = true; break; }
        }

        if(!seen) sources.push(source);
    }

    for(auto source: sources) {
        auto block = base[source];
        auto terminator = base[block->terminator];
        U32 edges = 0;

        if(terminator->kind == LowerInst::Je) {
            auto je = (LowerInstJe*)terminator;
            if(je->then == from - base) { je->then = to - base; edges++; }
            if(je->otherwise == from - base) { je->otherwise = to - base; edges++; }
        } else if(terminator->kind == LowerInst::Jmp) {
            auto jmp = (LowerInstJmp*)terminator;
            if(jmp->then == from - base) { jmp->then = to - base; edges++; }
        }

        for(auto& successor: block->outgoing) {
            if(successor == from - base) successor = to - base;
        }

        for(U32 i = 0; i < edges; i++) to->incoming.push(arena, source);
    }

    while(from->incoming.size()) from->incoming.remove(base, from->incoming.size() - 1);
}

// Taking a block nothing reaches any more out of the function. Its instructions stop counting as
// readers of what they read, so whatever they were the last use of is collectable - by the sweeps
// this pass runs behind, which is why it does not run one of its own. Renumbering is not optional:
// `index` is a position in this list and half the analyses index arrays by it.
static void dropBlock(LowerBase base, LowerFunction& fun, LowerBlock* block) {
    for(auto instPtr: block->instructions.contents(base)) detach(base, base[instPtr]);
    if(block->terminator) detach(base, base[block->terminator]);

    for(Size i = 0; i < fun.blocks.size(); i++) {
        if(fun.blocks.get(base, i) != block - base) continue;

        fun.blocks.remove(base, i);
        break;
    }

    for(Size i = 0; i < fun.blocks.size(); i++) base[fun.blocks.get(base, i)]->index = BlockIndex(i);
}

/*
 * One group of copies, collapsed into the first of them.
 *
 * The phis are built before anything moves, because each of them needs the operand every copy still
 * holds - including the surviving one's, which becomes the alternative on its own edges and would
 * otherwise be overwritten by the phi that is meant to select it.
 */
static void applyMerge(LowerBase base, LowerModule& module, LowerFunction& fun, LowerBlock* into,
                       SmallArray<LowerBlock*, 8>& from, SmallArray<Slot, 4>& differences)
{
    auto& arena = fun.arena;

    // Every edge the merged block will have, and which copy each one arrives from. `redirectEdges`
    // moves them below; what is collected here is the multiset it will produce, which is what the
    // phis are filled against.
    SmallArray<LowerPtr<LowerBlock>, 8> sources;
    SmallArray<LowerBlock*, 8> owners;

    for(auto source: into->incoming.contents(base)) {
        sources.push(source);
        owners.push(into);
    }

    for(auto block: from) {
        for(auto source: block->incoming.contents(base)) {
            sources.push(source);
            owners.push(block);
        }
    }

    SmallArray<LowerInstPhi*, 4> carried;

    for(auto& slot: differences) {
        auto at = instructionAt(base, into, slot.instruction);
        auto phi = makePhi(arena, base[at->used().ptr[slot.operand]]->type, sources.size());
        phi->source = at->source;

        auto used = phi->used();
        auto blocks = phi->sources();

        for(U32 i = 0; i < sources.size(); i++) {
            used[i] = instructionAt(base, owners[i], slot.instruction)->used().ptr[slot.operand];
            blocks[i] = sources[i];
        }

        carried.push(phi);
    }

    // Attached one at a time and read back one at a time: `addInst` is what registers a phi's own
    // reads, and the operand rewrite behind it is what makes the instruction read the phi instead of
    // the value it was holding.
    for(U32 i = 0; i < carried.size(); i++) {
        into->addInst(base, carried[i]);

        auto at = instructionAt(base, into, differences[i].instruction);
        setOperand(base, arena, at, at->used().ptr[differences[i].operand], &carried[i]->result);
    }

    for(auto block: from) {
        redirectEdges(base, arena, block, into);
        dropBlock(base, fun, block);
    }
}

} // namespace

void mergeDuplicatedExits(LowerBase base, LowerModule& module, LowerFunction& fun) {
    // Two exits and something to branch to them with. Below that there is nothing to merge, and the
    // check is here so that the common single-block function costs one comparison.
    if(fun.blocks.size() < 3) return;

    // The candidates, in block order, so that the copy that survives is the first one written - which
    // leaves the order of what is left as it was.
    //
    // Never the entry: nothing may branch to it, so it has no edges to redirect and no other block
    // may take its place. And never one nothing reaches, which is a block the x64 ordering already
    // asserts does not exist rather than something to remove quietly here.
    SmallArray<LowerBlock*, 16> exits;

    for(Size i = 1; i < fun.blocks.size(); i++) {
        auto block = base[fun.blocks.get(base, i)];
        if(!isExit(base, block) || block->incoming.isEmpty()) continue;

        exits.push(block);
    }

    if(exits.size() < 2) return;

    /*
     * A group at a time rather than a pair at a time, and that is what the phis cost: a phi's
     * alternatives are allocated with it, so the number of edges the merged block will have has to
     * be known before the first one is built. So the whole group is settled first and applied after.
     *
     * Which is also why the gate cannot be asked pairwise. Four copies of `call reclaim ; ret <n>`
     * are worth merging and any *two* of them are not - two instructions saved against a phi on two
     * edges is a wash, and it is only the third and fourth copies that make it pay. Asked one
     * candidate at a time, the group would be turned down at the first and never reach the fourth.
     *
     * So the copies that agree about everything are taken first and unconditionally - no
     * disagreement is no phi, and those merges are the ones this pass has always made - and the ones
     * that disagree are then priced as a single batch. All of them or none: a profitable subset of a
     * batch that is not profitable whole is a search, and what it would buy over declining is one
     * block in a shape nothing has produced.
     */
    for(Size i = 0; i < exits.size(); i++) {
        auto into = exits[i];
        if(!into) continue;

        SmallArray<LowerBlock*, 8> group;   // the copies that agree about everything
        SmallArray<Size, 8> differing;      // and the positions of the ones a phi would have to carry
        SmallArray<Slot, 4> differences;

        auto size = into->instructions.size() + 1;
        auto edges = into->incoming.size();

        for(Size j = i + 1; j < exits.size(); j++) {
            auto candidate = exits[j];
            if(!candidate) continue;

            // No predecessor may branch to two members of the group: retargeting would leave a `je`
            // whose two arms name one block, which `addInst` asserts against and which is a second
            // rewrite besides. Asked of everything collected, including the batch that may yet be
            // declined - which only ever refuses a merge that would otherwise have been legal.
            if(sharesAPredecessor(base, into, candidate)) continue;

            auto shared = false;
            for(auto member: group) {
                if(sharesAPredecessor(base, member, candidate)) { shared = true; break; }
            }

            for(auto position: differing) {
                if(shared) break;
                if(sharesAPredecessor(base, exits[position], candidate)) shared = true;
            }

            if(shared) continue;

            SmallArray<Slot, 4> found;
            if(!interchangeable(base, into, candidate, found)) continue;

            edges += candidate->incoming.size();

            if(found.isEmpty()) {
                // Taken out of the pool as it is accepted, which is what the identical case has
                // always done. The disagreeing ones stay in it until the batch is priced.
                group.push(candidate);
                exits[j] = nullptr;
                continue;
            }

            differing.push(j);
            for(auto& slot: found) {
                if(!holdsSlot(differences, slot)) differences.push(slot);
            }
        }

        if(differing.isNotEmpty() &&
           worthMerging(group.size() + differing.size() + 1, size, differences.size(), edges))
        {
            for(auto position: differing) {
                group.push(exits[position]);
                exits[position] = nullptr;
            }
        } else {
            // Left in the pool, so a later representative may still find a group they pay for.
            differences.clear();
        }

        if(group.isEmpty()) continue;

        applyMerge(base, module, fun, into, group, differences);
    }
}
