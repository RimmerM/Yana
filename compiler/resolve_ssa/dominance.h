#pragma once

#include "inst.h"

namespace ssa {

/*
 * The block order and the dominator tree, for everything at this tier that needs one.
 *
 * Beside `verify.cpp` rather than inside the optimizer directory, because the verifier runs in
 * builds the optimizer is switched off in and the lowering runs in every build: a shared analysis
 * that lived under `opt/` would be a dependency in the wrong direction. `opt/` caches these; the
 * verifier and the lowering build their own, uncached, and the verifier deliberately so - a cache
 * shared between a check and the thing it checks is not a check.
 */

// Every block one block's terminator can reach. A callback rather than a list, because the walks
// that ask for it ask once per block per visit and a list would be an allocation each time.
template<class F>
void forEachSuccessor(SsaBase base, Block& block, F&& f) {
    auto last = block.terminator(base);
    if(!last) return;

    forEachEdge(base, *last, [&](Edge& edge) {
        if(edge.target) f(base[edge.target]->index.index);
    });
}

/*
 * The body's blocks in reverse postorder from the entry, into `order`.
 *
 * Unreachable blocks are **absent**, which is the whole of what the order says about them: the walk
 * starts at the entry and follows edges, so a block nothing reaches is one the caller has to have a
 * separate answer for. `Dominators` reads that absence as "no position"; `Lowering::lowerBody` reads
 * it as "nothing jumps here, so nothing below needs its instructions".
 *
 * A postorder by an explicit stack rather than by recursion: a body is not bounded by anything that
 * would make a walk of its CFG safe on the C++ stack.
 */
void reversePostorder(SsaBase base, Body& body, Array<U32>& order);

/*
 * The dominator tree, by the iterative algorithm over reverse postorder.
 *
 * Needed and not optional. "Defined before used" in a graph is *dominance*, and the walk this
 * replaced approximated it with block index order - which is neither sound nor complete: a value
 * defined in one arm of a branch and used in the other has a lower index and does not dominate, and
 * a loop body legitimately uses a value defined in a block it precedes in index order. Both were
 * silent.
 *
 * It is also what makes the linear rule checkable: an owned value's consumption dataflow is only
 * meaningful over the blocks its definition dominates, and without that a value defined inside a
 * loop conflicts with itself across the back edge.
 */
struct Dominators {
    // Reverse postorder, and the position of each block in it. Unreachable blocks are absent from
    // the order and have no position, which is how the walk skips them without a second flag.
    Array<U32> order;
    Array<U32> position;
    Array<U32> idom;

    /*
     * The dominator tree, as an interval per block - what makes `dominates` a comparison.
     *
     * `a` dominates `b` exactly when `b` is in `a`'s subtree of the dominator tree, so a preorder
     * walk stamping each block with the range its subtree spans turns the question into two integer
     * comparisons. The walk this replaces climbed `b`'s idom chain to the root, and it is asked once
     * per operand of the body plus once per pair the linear rule compares - which is the
     * `uses x blocks` term in the bound over the verifier.
     *
     * The children are one CSR table rather than a list per block, which is the containers guidance
     * answer for a table indexed by block: two arrays reset per body instead of an allocation per
     * block per function.
     */
    Array<U32> childStart;
    Array<U32> childList;
    Array<U32> enter;
    Array<U32> leave;

    static constexpr U32 kNone = ~U32(0);

    void build(SsaBase base, Body& body);

    bool isReachable(U32 index) const { return position[index] != kNone; }

    // Two comparisons: `b` is dominated by `a` exactly where it sits inside `a`'s subtree.
    bool dominates(U32 a, U32 b) const {
        if(!isReachable(a) || !isReachable(b)) return false;
        return enter[a] <= enter[b] && leave[b] <= leave[a];
    }

private:
    // The two blocks' common dominator, by walking both up the tree until they meet - the standard
    // step of the Cooper/Harvey/Kennedy formulation, over reverse-postorder numbers.
    U32 intersect(U32 a, U32 b) const {
        while(a != b) {
            while(position[a] > position[b]) a = idom[a];
            while(position[b] > position[a]) b = idom[b];
        }

        return a;
    }

    // The children of each block in the dominator tree, then a preorder walk of it stamping the
    // interval each subtree spans.
    void numberTree(U32 count, U32 entry);
};

} // namespace ssa
