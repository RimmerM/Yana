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
 * The four kinds that carry something else - `Imm`, `Fun`, `Global`, `Alloca` - are refused here and
 * three of them are compared by `sameExitOperand` instead, which is where they are actually reached
 * from: all three are built in the constant block rather than in an arm. **`Alloca` is the refusal
 * that is a decision rather than an omission.** Two copies of an exit that each allocate are two
 * distinct pieces of storage and only one of them ever runs, so unifying them would be sound - but a
 * frame slot is the one thing here whose identity outlives the block, and no exit block allocates in
 * any program this compiler emits.
 */
static bool comparableByFlags(LowerInst* inst) {
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
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
        case LowerInst::Cmp:
        case LowerInst::Select:
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

static bool sameExitInstruction(LowerBase base, LowerBlock* left, LowerBlock* right,
                                LowerInst* a, LowerInst* b)
{
    if(a->kind != b->kind) return false;
    if(a->flags != b->flags) return false;
    if(a->createdCount != b->createdCount || a->usedCount != b->usedCount) return false;
    if(!comparableByFlags(a)) return false;

    auto madeA = a->created();
    auto madeB = b->created();
    for(U32 i = 0; i < madeA.length; i++) {
        if(madeA.ptr[i].type != madeB.ptr[i].type) return false;
    }

    auto usedA = a->used();
    auto usedB = b->used();
    for(U32 i = 0; i < usedA.length; i++) {
        if(!sameExitOperand(base, left, right, usedA.ptr[i], usedB.ptr[i])) return false;
    }

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
 * Whether one block may stand in for the other.
 *
 * The belt at the end is the one thing here that is not a comparison. A block with no successors
 * dominates nothing, so no value it defines can be read outside it - that is the argument the whole
 * pass rests on, and it is an argument about the IR being in SSA form rather than something this
 * file established. Asking the use lists directly costs one walk of a two-instruction block and
 * turns a broken invariant somewhere else into a merge that does not happen.
 */
static bool interchangeable(LowerBase base, LowerBlock* a, LowerBlock* b) {
    if(a->phis.size() || b->phis.size()) return false;
    if(a->instructions.size() != b->instructions.size()) return false;
    if(a->instructions.size() > kMaxExitSize) return false;

    auto instsA = a->instructions.contents(base);
    auto instsB = b->instructions.contents(base);

    for(U32 i = 0; i < instsA.size(); i++) {
        if(!sameExitInstruction(base, a, b, base[instsA[i]], base[instsB[i]])) return false;
    }

    if(!sameExitInstruction(base, a, b, base[a->terminator], base[b->terminator])) return false;

    for(auto instPtr: instsB) {
        for(auto& value: base[instPtr]->created()) {
            for(auto user: value.uses.contents(base)) {
                if(base[base[user]->block] != b) return false;
            }
        }
    }

    return true;
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

} // namespace

void mergeIdenticalExits(LowerBase base, LowerModule& module, LowerFunction& fun) {
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

    // Applied as they are found rather than collected first, so that every question below is asked of
    // the edges as the merges above it left them - `sharesAPredecessor` in particular, which is about
    // the predecessor list of a block two earlier merges may already have moved edges onto.
    for(Size i = 0; i < exits.size(); i++) {
        for(Size j = 0; j < i; j++) {
            auto into = exits[j];
            if(!into) continue;
            if(sharesAPredecessor(base, into, exits[i])) continue;
            if(!interchangeable(base, into, exits[i])) continue;

            redirectEdges(base, module.arena, exits[i], into);
            dropBlock(base, fun, exits[i]);
            exits[i] = nullptr;
            break;
        }
    }
}
