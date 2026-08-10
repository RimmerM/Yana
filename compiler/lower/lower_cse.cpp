#include "lower_cse.h"
#include "lower_builder.h"

namespace {

bool sameComputation(LowerBase base, LowerInst* a, LowerInst* b, Size depth);

/*
 * How far `sameOperand` looks below the operands it is comparing.
 *
 * The shape it has to reach is a checked subscript, whose address is `add %items, (shl (cast %i))` -
 * three deep, and four with the extension of a narrower index below it. Past that the answer stops
 * paying: what is being asked is whether two instructions the walk has *not* unified compute the
 * same thing, and two chains that agree for five levels and were still written twice do not occur in
 * IR this pass has already been over once.
 */
static constexpr Size kMaxOperandDepth = 4;

/*
 * Whether two operands are the same value.
 *
 * Identity first, then two things beside it.
 *
 * Two immediates of one type holding one number. They are not shared - `immediate()` in
 * resolve/lower_type.cpp builds a fresh `Imm` per call, and the fold and the strength reduction each
 * build their own - so comparing operands by identity alone says that the two `add %frame, 12`s
 * under two reads of one field are two different computations, which is most of what this pass
 * exists to collect. The comparison is on the immediate's stored word, which for a floating one is
 * its bits: `LowerImm` holds the two in a union, so this is a bitwise equality either way and `-0.0`
 * never reads as `0.0`.
 *
 * And two *pure computations* that agree, which is the recursive half and the one the branch fold
 * and the load unification below both rest on. A checked subscript reached twice writes its index
 * extension, its scale and its base addition out twice, and the second copy is only recognisable as
 * the first one once those are compared by what they compute rather than by which instruction
 * computed them. The walk unifies them where it may, but it may not always - `answerableFrom`
 * declines a value a loop header computes, and a checked loop's index extension is exactly that -
 * so a question asked only of the values the walk happened to unify would miss the case it is for.
 *
 * The recursion goes through `isRepeatable` only, which is to say through arithmetic and never
 * through a load. Two loads of one address are the same value only where nothing wrote between them,
 * and that is a fact about a *path* rather than about the two instructions - `availableLoads` below
 * is what holds it. Descending into one here would be asserting it without having asked.
 */
bool sameOperand(LowerBase base, LowerPtr<LowerValue> first, LowerPtr<LowerValue> second,
                 Size depth = 0)
{
    if(first == second) return true;
    if(!first || !second) return false;

    auto a = base[first];
    auto b = base[second];
    if(a->type != b->type) return false;

    auto left = a->inst();
    auto right = b->inst();

    if(left->kind == LowerInst::Imm && right->kind == LowerInst::Imm) {
        return ((LowerImm*)left)->i == ((LowerImm*)right)->i;
    }

    if(depth >= kMaxOperandDepth) return false;
    if(!isRepeatable(left) || !isRepeatable(right)) return false;

    return sameComputation(base, left, right, depth + 1);
}

// Commutative in the sense this pass needs: the operands may be compared as a pair rather than in
// order. A comparison is not one of these - swapping its operands is a different comparison unless
// the test is swapped with them, and there is nothing here to swap it against.
bool isCommutative(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Add: case LowerInst::Mul: case LowerInst::IMul:
        case LowerInst::And: case LowerInst::Or:  case LowerInst::Xor:
            return true;
        default:
            return false;
    }
}

/*
 * Whether two repeatable instructions compute the same thing.
 *
 * Everything a kind carries beside its operands is compared here. `LowerInstCast`'s signedness is
 * the one that is not obvious and the one that matters: the same source at the same result type
 * sign-extends or zero-extends according to two flag bits, and two casts that disagree about them
 * produce two different numbers. `skipsExtend` is deliberately *not* compared - it is the backend's
 * note to itself about an encoding and is zero everywhere this pass can be reached from.
 */
/*
 * §31.5 Whether two instructions producing these types produce the same *bytes*.
 *
 * Ordinarily that is type equality, and for everything but a load it is exactly that: an operation's
 * type is what it computes in, so an `add` of two `Int32`s and an `add` of two `Int64`s are two
 * different numbers even where their operands agree.
 *
 * A load computes nothing. Its width and its signedness say which bytes arrive and how they are
 * extended, and its type says only which register class they arrive in - so two loads of one address
 * at one width differ in nothing when their types are the two that share a class. `Int64` and
 * `Pointer` are that pair and the only one: `Int32` against `Int64` is a different register view of
 * a different number of bits, and an integer against a float is a different bank.
 *
 * Which matters because a niche-encoded option is read twice. `Maybe(Tree)` is one word - a null
 * pointer for `Nothing` and the payload otherwise - so `if node.left is Just(l)` is a load of the
 * discriminant against zero and then a load of the payload, at one address and one width, typed
 * `Int64` and `Pointer`. Every match on one paid a second load of a word it had just read, in
 * `depthOf` and `total` in `test/bench/programs/Tree.yana` and in every `is Just` in the language.
 *
 * The identity is a fact about the *Repr* - it holds because the niche put the discriminant in the
 * payload's own bits - and neither reader knows that. What they agree on is the address, which is
 * what this reads it off.
 */
static bool sameLoadedClass(LowerInst::Kind kind, LowerType a, LowerType b) {
    if(a == b) return true;
    if(kind != LowerInst::Load) return false;

    auto wordClass = [](LowerType t) { return t == LowerType::Int64 || t == LowerType::Pointer; };
    return wordClass(a) && wordClass(b);
}

bool sameComputation(LowerBase base, LowerInst* a, LowerInst* b, Size depth = 0) {
    if(a->kind != b->kind) return false;

    auto left = ((LowerInstSingle*)a)->created().ptr;
    auto right = ((LowerInstSingle*)b)->created().ptr;
    if(!sameLoadedClass(a->kind, left->type, right->type)) return false;

    auto same = [&](LowerPtr<LowerValue> x, LowerPtr<LowerValue> y) {
        return sameOperand(base, x, y, depth);
    };

    // A load says which storage it reads in its width and its signedness as well as in its address,
    // and two that disagree about either are two different numbers out of the same bytes. Whether
    // the storage still holds what the first one read is not asked here - see `availableLoads`.
    if(a->kind == LowerInst::Load) {
        auto loadA = (LowerInstLoad*)a;
        auto loadB = (LowerInstLoad*)b;

        if(loadA->getWidth() != loadB->getWidth()) return false;
        if(loadA->isSigned() != loadB->isSigned()) return false;
        return same(loadA->from, loadB->from);
    }

    if(a->kind == LowerInst::Cast) {
        auto castA = (LowerInstCast*)a;
        auto castB = (LowerInstCast*)b;

        if(castA->isSignedSource() != castB->isSignedSource()) return false;
        if(castA->isSignedResult() != castB->isSignedResult()) return false;
    }

    if(a->kind == LowerInst::Select) {
        auto selectA = (LowerInstSelect*)a;
        auto selectB = (LowerInstSelect*)b;

        // The comparison a target folded into the select, which is held in `flags` and is zero
        // everywhere this pass can be reached from. Compared as the encoded word rather than as the
        // `Maybe` it decodes to, so that a target that starts setting it cannot be forgotten here.
        if(selectA->flags != selectB->flags) return false;
        return same(selectA->lhs, selectB->lhs) && same(selectA->rhs, selectB->rhs) &&
               same(selectA->cmp, selectB->cmp);
    }

    if(a->kind == LowerInst::Cmp) {
        auto cmpA = (LowerInstCmp*)a;
        auto cmpB = (LowerInstCmp*)b;

        if(cmpA->getCmp() != cmpB->getCmp()) return false;
        return same(cmpA->lhs, cmpB->lhs) && same(cmpA->rhs, cmpB->rhs);
    }

    if(isUnary(a) || isCast(a)) {
        return same(((LowerInstUnary*)a)->from, ((LowerInstUnary*)b)->from);
    }

    auto binaryA = (LowerInstBinary*)a;
    auto binaryB = (LowerInstBinary*)b;

    if(isCommutative(a) && same(binaryA->lhs, binaryB->rhs) && same(binaryA->rhs, binaryB->lhs)) {
        return true;
    }

    return same(binaryA->lhs, binaryB->lhs) && same(binaryA->rhs, binaryB->rhs);
}

/*
 * Whether one of these may be answered from an earlier one in *another* block.
 *
 * A comparison may not. Its result is the flags, which the branch below it consumes where it stands;
 * a second reader somewhere else turns it into a `setcc` into a register, a register that has to
 * live from here to there, and a `test` at each use. That is §2 of test/bench/findings.md arriving
 * from the other direction - the pass that is meant to remove a materialization would be inserting
 * one - and it costs more than the compare it saves. Within one block the two readers are already
 * reading one set of flags, so there it is kept.
 */
bool answerableAcrossBlocks(LowerInst* inst) {
    return inst->kind != LowerInst::Cmp;
}

/*
 * Whether a call between two computations is a reason not to unify them.
 *
 * This is the pass's whole cost, and it is the one §9.1 already wrote down about promotion: a value
 * that does not get a register is worse than the computation it replaced. Recomputing costs one
 * instruction; carrying a value across a call costs a callee-saved register the allocator may not
 * have, and when it does not, a store and a reload at every use. `countPrimes` in
 * test/bench/programs/Sieve.yana is what measured it - unifying the `count + 1` on either side of the
 * `reserve` call inside `push` added three stores and two reloads to a loop that runs once per
 * element, for one `inc` saved, and cost 6%.
 *
 * The clobber set is what decides it, so the convention is the question rather than the callee. An
 * ordinary call may write nearly every register, so a value live across one has to survive in the
 * handful that are preserved. A *syscall* does not - see §9.5 of findings.md, where the kernel
 * keeping its argument registers is the whole of that item - so a value live across one competes for
 * far less, and declining there would cost the bounds-checked loops this pass exists for: an abort
 * arm is a syscall, and it sits between every check and the one after it.
 */
bool costsARegister(LowerInst* inst) {
    if(inst->kind != LowerInst::Call) return false;
    return ((LowerInstCall*)inst)->getCallType() != LowerCallType::Syscall;
}

/*
 * Whether this may have written storage some earlier load read.
 *
 * The whole memory model this pass has, and it is deliberately the coarsest one: any write retires
 * every load in scope rather than the loads that could alias it. There is no place information at
 * this tier - a `Load` names an address that arithmetic produced, and two addresses are the same
 * question this pass is being asked rather than one it can answer - so an alias rule here would be
 * either "the same address" (which is already the unification test) or a guess.
 *
 * A call retires them because a callee may write anything it was handed, and a *syscall* does too,
 * which is the one place this and `costsARegister` disagree: the kernel keeping its argument
 * registers says nothing about what it does to memory, and `read` into a buffer is a write. It costs
 * nothing here - the syscall that sits between every check and the one below it is an abort arm, a
 * block with no successors, so it is never on a path between two loads.
 */
bool writesStorage(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Store:
        case LowerInst::Copy:
        case LowerInst::SetPattern:
        case LowerInst::Call:
            return true;
        default:
            return false;
    }
}

/*
 * A value a loop *header* computes, answered from anywhere but that header - declined, and this is
 * the rule the whole difference between the pass paying and costing turns on.
 *
 * A header asks a question. What it computes is the loop's test and the operands of it, and those
 * die at the branch below them - so a reader anywhere else keeps the value alive across the body,
 * which for a nested loop is the whole of the inner one. That is the same trade the promotion item
 * lost on, one level up: a computation recovered costs an instruction, and a live range across a
 * loop costs a register the allocator may not have.
 *
 * It is also exactly the shape the x64 backend's loop rotation declines to rotate - see
 * `isRotatableHeaderInst` in codegen/x64/transform.cpp, whose "a header instruction read anywhere but
 * the header is declined" is this sentence read from the other end. So a header value handed to the
 * body does not merely cost a register; it costs the rotation as well, which is a jump per iteration
 * of the loop it belongs to.
 *
 * `countPrimes` in test/bench/programs/Sieve.yana is what measured it, and it is the whole of that
 * program's 4.7%: `while p * p < limit` and `let &m = p * p` are the same computation, one in the
 * header and one in the block below it, and unifying them saved one `imul` per outer iteration in
 * exchange for `p * p` living across every inner iteration and the outer loop losing its rotation.
 *
 * `multiply` in Matrix.yana is the case this must *not* refuse, and shows why the rule is about the
 * header rather than about loop depth: `row * n` is computed in the k-loop's **body**, and reusing it
 * in the column loop below is worth 5%. The two are the same "reuse across a loop level" and only one
 * of them is a header.
 */
bool answerableFrom(LowerBase base, const LoopInfo& loops, LowerInst* candidate, LowerInst* inst) {
    auto from = base[candidate->block]->index;
    if(!loops.isHeader(from)) return true;

    return from == base[inst->block]->index;
}

struct Eliminator {
    LowerBase base;
    LowerModule& module;
    LowerFunction& fun;
    const LoopInfo& loops;
    const DominatorTree& dominance;

    // The dominator tree's edges, indexed by postorder offset as everything derived from
    // `DominatorTree` is - `tree` holds each block's immediate dominator and this is that read the
    // other way round, which is the direction a walk needs.
    ArrayList<BlockIndex, 4> children;

    // Per block and indexed by postorder offset: whether it contains a call worth not crossing, and
    // whether it writes storage at all. Two questions rather than one because the two lists below
    // are retired on different events - see `writesStorage`.
    SmallArray<bool, 64> calls;
    SmallArray<bool, 64> writes;

    /*
     * The computations on the path from the entry to the block being visited. Every one of them
     * dominates it, which is what makes it an answer.
     *
     * `retired` is how far into the list the calls have reached: everything below it was computed
     * before one and is no longer offered. It only ever grows within a subtree and is restored on the
     * way out, because a call on one arm of a branch says nothing about the other.
     */
    SmallArray<LowerPtr<LowerInst>, 32> available;
    Size retired = 0;

    /*
     * The same for the loads, kept apart from the computations because what retires the two is not
     * the same event.
     *
     * A computation survives anything but a call it would have to be held across; a load survives
     * only until something writes storage, which a store does and a call does too. Held in one list
     * with the wider watermark, every load a store retired would take the arithmetic in front of it
     * with it - and the arithmetic is the larger half of what this pass collects.
     */
    SmallArray<LowerPtr<LowerInst>, 32> loads;
    Size loadsRetired = 0;

    // Everything that could run between a block's immediate dominator and the block itself. A path
    // to here goes through that dominator by definition, so anything reachable backwards from a
    // predecessor without passing it is on such a path - and a loop header finds its own body,
    // because the latch is one of its predecessors.
    //
    // Both questions are answered by one walk, since the walk is the expensive part and the two
    // differ only in which flag they read.
    struct Between { bool call = false; bool write = false; };

    Between between(BlockIndex postIndex) {
        SmallArray<BlockIndex, 32> pending;
        SmallArray<bool, 64> seen;
        for(Size i = 0; i < dominance.postorder.size(); i++) seen.push(false);

        auto stop = dominance.tree[postIndex];
        auto reach = [&](BlockIndex at) {
            if(at == stop || seen[at]) return;

            seen[at] = true;
            pending.push(at);
        };

        auto block = base[fun.blocks.get(base, dominance.postorder[postIndex])];
        for(auto pred: block->incoming.contents(base)) reach(base[pred]->postIndex);

        Between found;
        while(pending.size()) {
            auto at = pending.pop().unwrap();
            if(calls[at]) found.call = true;
            if(writes[at]) found.write = true;
            if(found.call && found.write) return found;

            for(auto pred: base[fun.blocks.get(base, dominance.postorder[at])]->incoming.contents(base)) {
                reach(base[pred]->postIndex);
            }
        }

        return found;
    }

    /*
     * What a branch above this block has already decided, one entry per arm the walk is inside.
     *
     * Pushed when the walk descends from a block into a dominator-tree child that is one of its two
     * successors *and has no other predecessor*: every path that reaches the child left the parent
     * by that edge, so the parent's question has one answer for the whole of the child's subtree.
     * That is the whole of the reasoning, and the one-predecessor test is what makes it sound - a
     * child reached from elsewhere as well is reached on a path the branch says nothing about.
     */
    struct BranchFact {
        LowerPtr<LowerInst> cmp;
        bool holds;
    };

    SmallArray<BranchFact, 16> facts;

    // A branch whose answer is already known, and the successor it therefore always takes. Applied
    // after the walk rather than during it, so that the dominator tree the walk is reading stays the
    // tree the function has - see `eliminateCommonValues`.
    struct DecidedBranch {
        LowerPtr<LowerBlock> block;
        bool takesThen;
    };

    SmallArray<DecidedBranch, 8> decided;

    bool changed = false;

    /*
     * Whether this block's branch asks a question the walk is already inside the answer to.
     *
     * The comparison is by what is computed rather than by which value computes it, which is the
     * point: the second bounds check of one index against one length is written as a second `cmp` of
     * a second sign extension, and no earlier pass unified either - `answerableFrom` declines the
     * extension because a checked loop computes it in its header, and `answerableAcrossBlocks`
     * declines the comparison because a `cmp` read from another block has to be materialized. So the
     * two are the same check and are not the same value, and only `sameComputation` says so.
     */
    Maybe<bool> decidedBranch(LowerBlock* block) {
        if(!block->terminator) return Nothing();

        auto terminator = base[block->terminator];
        if(terminator->kind != LowerInst::Je) return Nothing();

        auto condition = base[((LowerInstJe*)terminator)->cond]->inst();
        if(condition->kind != LowerInst::Cmp) return Nothing();

        for(Size i = facts.size(); i-- > 0;) {
            auto known = base[facts[i].cmp];
            if(sameComputation(base, known, condition)) return Just(facts[i].holds);
        }

        return Nothing();
    }

    void run(BlockIndex postIndex) {
        auto scope = available.size();
        auto retiredScope = retired;
        auto loadScope = loads.size();
        auto loadsRetiredScope = loadsRetired;
        auto factScope = facts.size();
        auto block = base[fun.blocks.get(base, dominance.postorder[postIndex])];

        auto crossed = between(postIndex);
        if(crossed.call) retired = available.size();
        if(crossed.write) loadsRetired = loads.size();

        // Inline: one of these per block, holding the instructions of that block while the list it
        // came from is rewritten - the same shape as foldFunctionConstants, and for the same reason.
        SmallArray<LowerPtr<LowerInst>, 32> kept;
        auto rewrote = false;

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            if(costsARegister(inst)) retired = available.size();
            if(writesStorage(inst)) loadsRetired = loads.size();

            auto isLoad = inst->kind == LowerInst::Load;
            if(!isLoad && !isRepeatable(inst)) {
                kept.push(instPtr);
                continue;
            }

            auto& pool = isLoad ? loads : available;
            auto floor = isLoad ? loadsRetired : retired;

            LowerPtr<LowerInst> existing = nullptr;
            for(Size i = pool.size(); i-- > floor;) {
                auto candidate = base[pool[i]];
                if(!sameComputation(base, candidate, inst)) continue;
                if(!answerableAcrossBlocks(inst) && candidate->block != inst->block) continue;
                if(!answerableFrom(base, loops, candidate, inst)) continue;

                existing = pool[i];
                break;
            }

            if(!existing) {
                pool.push(instPtr);
                kept.push(instPtr);
                continue;
            }

            auto removed = ((LowerInstSingle*)inst)->created().ptr;
            auto survivor = ((LowerInstSingle*)base[existing])->created().ptr;

            detach(base, inst);

            /*
             * §31.5 The two 64-bit classes, reconciled - see sameLoadedClass, which is what let a
             * `Pointer` load and an `Int64` one unify in the first place.
             *
             * The bytes are the same and the *declared type* is not, and later passes read that
             * type: `peelAddress` folds arithmetic on a `Pointer` and declines it on an `Int64`,
             * and the verifier requires an argument to match its parameter. So the load is replaced
             * by a bitcast of the survivor rather than by the survivor itself, which says the same
             * thing at the type the readers were written against.
             *
             * It costs nothing. `FormBitcast` between two integer classes is `mov r, r` with
             * `omitWhenSame`, and the two values are a copy apart - so coalescing gives them one
             * register and the instruction emits no bytes at all.
             *
             * Built and threaded by hand rather than through addInst, because the block's list is
             * being rebuilt from `kept` underneath this loop and an append would be discarded.
             */
            if(removed->type != survivor->type) {
                auto cast = new (module.arena) LowerInstUnary(
                    LowerInst::Bitcast, removed->name, removed->type, survivor - base
                );

                cast->block = block - base;
                survivor->uses.push(module.arena, LowerPtr<LowerInst>((LowerInst*)cast - base));

                survivor = ((LowerInstSingle*)cast)->created().ptr;
                kept.push(cast - base);
            }

            replaceUses(base, module.arena, removed - base, survivor - base);

            rewrote = true;
            changed = true;
        }

        if(rewrote) {
            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
        }

        if(auto known = decidedBranch(block)) decided.push(DecidedBranch { block - base, known.unwrap() });

        for(auto child: children[postIndex]) {
            auto pushed = pushFact(block, base[fun.blocks.get(base, dominance.postorder[child])]);
            run(child);
            if(pushed) facts.pop();
        }

        while(available.size() > scope) available.pop();
        while(loads.size() > loadScope) loads.pop();
        while(facts.size() > factScope) facts.pop();
        retired = retiredScope;
        loadsRetired = loadsRetiredScope;
    }

    // See `facts`. The child has to be one of the two arms and to be reachable through no other
    // edge, which together say that every path into it took that arm.
    bool pushFact(LowerBlock* block, LowerBlock* child) {
        if(!block->terminator || base[block->terminator]->kind != LowerInst::Je) return false;
        if(child->incoming.size() != 1) return false;

        auto je = (LowerInstJe*)base[block->terminator];
        auto condition = base[je->cond]->inst();
        if(condition->kind != LowerInst::Cmp) return false;

        auto holds = je->then == child - base;
        if(!holds && je->otherwise != child - base) return false;

        facts.push(BranchFact { condition - base, holds });
        return true;
    }
};

// Taking a block that nothing reaches out of the function. It has no successors - see the caller,
// which is the only one and only offers such a block - so there is no edge to unpick and no phi
// anywhere reading an alternative from it. Renumbering is not optional: `index` is a position in
// this list and half the analyses index arrays by it.
void removeBlock(LowerBase base, LowerFunction& fun, LowerBlock* block) {
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
 * Turning a branch whose answer is known into the jump it always was.
 *
 * The arm being dropped has to be a block **control never leaves** - an abort arm, or a `ret`. That
 * is a narrowing rather than the general rule, and it is what keeps this to the one thing it has to
 * do. A dropped arm with successors of its own can leave a whole region unreachable, and the x64
 * block ordering asserts that no such region exists (`assertTrue(postorder.size() == fun.blocks.size())`
 * in codegen/x64/transform.cpp), so anything more general owes a reachability sweep and a repair for
 * every phi in what it disconnected. A block with no outgoing edges owes neither: nothing names it
 * but the branch above, so once that branch stops naming it, it is gone.
 *
 * Which is exactly the shape the pass is for. A bounds check that a dominating check already decided
 * is a `je` whose other arm is a syscall and an `unreachable`, and there are two of those in every
 * loop that reads one element twice.
 */
bool takeDecidedArm(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun,
                    LowerBlock* block, bool takesThen)
{
    auto je = (LowerInstJe*)base[block->terminator];
    auto taken = takesThen ? je->then : je->otherwise;
    auto dropped = takesThen ? je->otherwise : je->then;

    auto droppedBlock = base[dropped];
    if(droppedBlock->outgoing[0] || droppedBlock->outgoing[1]) return false;

    // Unwired by hand rather than through `addInst`, which records an edge instead of replacing one
    // and refuses a successor that already names this block - and the taken one does, since it is
    // keeping the edge it already had. Leaving that edge alone is also what keeps the taken block's
    // phis reading the alternative they were built with.
    detach(base, je);
    block->terminator = nullptr;
    block->outgoing[0] = nullptr;
    block->outgoing[1] = nullptr;

    for(Size i = 0; i < droppedBlock->incoming.size(); i++) {
        if(droppedBlock->incoming.get(base, i) != block - base) continue;

        droppedBlock->incoming.remove(base, i);
        break;
    }

    auto jmp = new (arena) LowerInstJmp(taken);
    jmp->block = block - base;
    block->terminator = (LowerInst*)jmp - base;
    block->outgoing[0] = taken;

    // And the arm itself, once nothing reaches it. Its instructions stop counting as readers of what
    // they read, so the sweep behind this pass can collect whatever they were the last use of.
    if(droppedBlock->incoming.isEmpty()) removeBlock(base, fun, droppedBlock);
    return true;
}

} // namespace

bool eliminateCommonValues(LowerBase base, LowerModule& module, LowerFunction& fun,
                           const LoopAnalysis& analysis)
{
    if(fun.blocks.size() < 2) return false;

    // The caller's pair, built loops-first for the reason this line used to give itself: both walks
    // rebuild the postorder, and the tree's copy of it is the one every index below is read against.
    // See LoopAnalysis, where that ordering now lives.
    Eliminator eliminator { base, module, fun, analysis.loops, analysis.dominators };
    auto count = eliminator.dominance.postorder.size();
    if(!count) return false;

    eliminator.children.reset(count);

    // Everything but the entry, which is its own immediate dominator and would otherwise be its own
    // child - see buildDominatorTree, where `dominators[startNode] = startNode` says so.
    for(BlockIndex i = 0; i < BlockIndex(count); i++) {
        auto block = base[fun.blocks.get(base, eliminator.dominance.postorder[i])];

        auto hasCall = false;
        auto hasWrite = false;
        for(auto instPtr: block->instructions.contents(base)) {
            if(costsARegister(base[instPtr])) hasCall = true;
            if(writesStorage(base[instPtr])) hasWrite = true;
        }

        eliminator.calls.push(hasCall);
        eliminator.writes.push(hasWrite);
        if(i == eliminator.dominance.startIndex) continue;

        eliminator.children[eliminator.dominance.tree[i]].push(i);
    }

    eliminator.run(eliminator.dominance.startIndex);

    /*
     * And the arms, last - which is where the analysis above stops being the answer. Every one of
     * these removes an edge and most of them remove the block it led to, which renumbers the rest;
     * `loops` and `dominance` are indexed by exactly that numbering, and nothing below reads either.
     *
     * Counted rather than assumed, because a decided branch whose arm has successors of its own is
     * declined - see `takeDecidedArm` - and a function where every one of them was is one whose CFG
     * did not move.
     */
    auto rewired = false;
    for(auto& branch: eliminator.decided) {
        if(takeDecidedArm(base, module.arena, fun, base[branch.block], branch.takesThen)) {
            eliminator.changed = true;
            rewired = true;
        }
    }

    if(eliminator.changed) removeDeadValues(base, module.arena, fun);
    return rewired;
}
