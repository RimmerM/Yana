#include "lower_cse.h"
#include "lower_builder.h"

namespace {

/*
 * Whether two operands are the same value.
 *
 * Identity, and one thing beside it: two immediates of one type holding one number. They are not
 * shared - `immediate()` in resolve/lower_type.cpp builds a fresh `Imm` per call, and the fold and
 * the strength reduction each build their own - so comparing operands by identity alone says that
 * the two `add %frame, 12`s under two reads of one field are two different computations, which is
 * most of what this pass exists to collect.
 *
 * The comparison is on the immediate's stored word, which for a floating one is its bits: `LowerImm`
 * holds the two in a union, so this is a bitwise equality either way and `-0.0` never reads as `0.0`.
 */
bool sameOperand(LowerBase base, LowerPtr<LowerValue> first, LowerPtr<LowerValue> second) {
    if(first == second) return true;
    if(!first || !second) return false;

    auto a = base[first];
    auto b = base[second];
    if(a->type != b->type) return false;

    auto left = a->inst();
    auto right = b->inst();
    if(left->kind != LowerInst::Imm || right->kind != LowerInst::Imm) return false;

    return ((LowerImm*)left)->i == ((LowerImm*)right)->i;
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
bool sameComputation(LowerBase base, LowerInst* a, LowerInst* b) {
    if(a->kind != b->kind) return false;

    auto left = ((LowerInstSingle*)a)->created().ptr;
    auto right = ((LowerInstSingle*)b)->created().ptr;
    if(left->type != right->type) return false;

    auto same = [&](LowerPtr<LowerValue> x, LowerPtr<LowerValue> y) {
        return sameOperand(base, x, y);
    };

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
    LoopInfo loops;
    DominatorTree dominance;

    // The dominator tree's edges, indexed by postorder offset as everything derived from
    // `DominatorTree` is - `tree` holds each block's immediate dominator and this is that read the
    // other way round, which is the direction a walk needs.
    ArrayList<BlockIndex, 4> children;

    // Per block and indexed by postorder offset: whether it contains a call worth not crossing.
    SmallArray<bool, 64> calls;

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

    // Everything that could run between a block's immediate dominator and the block itself. A path
    // to here goes through that dominator by definition, so anything reachable backwards from a
    // predecessor without passing it is on such a path - and a loop header finds its own body,
    // because the latch is one of its predecessors.
    bool callBetween(BlockIndex postIndex) {
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

        while(pending.size()) {
            auto at = pending.pop().unwrap();
            if(calls[at]) return true;

            for(auto pred: base[fun.blocks.get(base, dominance.postorder[at])]->incoming.contents(base)) {
                reach(base[pred]->postIndex);
            }
        }

        return false;
    }

    bool changed = false;

    void run(BlockIndex postIndex) {
        auto scope = available.size();
        auto retiredScope = retired;
        auto block = base[fun.blocks.get(base, dominance.postorder[postIndex])];

        if(callBetween(postIndex)) retired = available.size();

        // Inline: one of these per block, holding the instructions of that block while the list it
        // came from is rewritten - the same shape as foldFunctionConstants, and for the same reason.
        SmallArray<LowerPtr<LowerInst>, 32> kept;
        auto rewrote = false;

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            if(costsARegister(inst)) retired = available.size();

            if(!isRepeatable(inst)) {
                kept.push(instPtr);
                continue;
            }

            LowerPtr<LowerInst> existing = nullptr;
            for(Size i = available.size(); i-- > retired;) {
                auto candidate = base[available[i]];
                if(!sameComputation(base, candidate, inst)) continue;
                if(!answerableAcrossBlocks(inst) && candidate->block != inst->block) continue;
                if(!answerableFrom(base, loops, candidate, inst)) continue;

                existing = available[i];
                break;
            }

            if(!existing) {
                available.push(instPtr);
                kept.push(instPtr);
                continue;
            }

            detach(base, inst);
            replaceUses(base, module.arena, ((LowerInstSingle*)inst)->created().ptr - base,
                        ((LowerInstSingle*)base[existing])->created().ptr - base);

            rewrote = true;
            changed = true;
        }

        if(rewrote) {
            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
        }

        for(auto child: children[postIndex]) run(child);

        while(available.size() > scope) available.pop();
        retired = retiredScope;
    }
};

} // namespace

void eliminateCommonValues(LowerBase base, LowerModule& module, LowerFunction& fun) {
    if(fun.blocks.size() < 2) return;

    // The loops before the dominator tree, because both rebuild the postorder and the tree's copy
    // of it is the one every index below is read against.
    Eliminator eliminator { base, module, fun, fun.buildLoops(base), fun.buildDominatorTree(base) };
    auto count = eliminator.dominance.postorder.size();
    if(!count) return;

    eliminator.children.reset(count);

    // Everything but the entry, which is its own immediate dominator and would otherwise be its own
    // child - see buildDominatorTree, where `dominators[startNode] = startNode` says so.
    for(BlockIndex i = 0; i < BlockIndex(count); i++) {
        auto block = base[fun.blocks.get(base, eliminator.dominance.postorder[i])];

        auto hasCall = false;
        for(auto instPtr: block->instructions.contents(base)) {
            if(costsARegister(base[instPtr])) { hasCall = true; break; }
        }

        eliminator.calls.push(hasCall);
        if(i == eliminator.dominance.startIndex) continue;

        eliminator.children[eliminator.dominance.tree[i]].push(i);
    }

    eliminator.run(eliminator.dominance.startIndex);

    if(eliminator.changed) removeDeadValues(base, module.arena, fun);
}
