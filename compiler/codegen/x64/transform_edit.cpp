#include "transform_internal.h"

/*
 * The edits a transform makes to the lower IR.
 *
 * Every pass in this directory rewrites instructions rather than reading them, and these are the
 * five ways it is allowed to: insert one, take one out, point a use somewhere else, move one, and
 * split the edge a move needs a block of its own on. They are here rather than in lower_builder.h
 * because each of them keeps the *use lists* agreeing with the change, which is the invariant the
 * passes below rely on and the one a hand-written edit forgets - see transform_internal.h for the
 * map of the files that call them.
 */

// Inserts an empty block on the edge from `pred` (its `outgoing[edge]`) to `succ`, so that the
// moves that feed `succ`'s phis have a block of their own to live in.
//
// Phi moves are emitted at the end of the predecessor, which is only sound if control reaching that
// point is guaranteed to continue into the phi's block. When the predecessor ends in a conditional
// branch, it is not: the moves would run on the way to *both* successors, writing phi registers on
// a path where they hold something else. Splitting gives the edge a block whose only successor is
// `succ`, which restores that guarantee.
static void splitEdge(LowerBase base, LowerFunction& fun, LowerBlock* pred, Size edge) {
    auto& arena = fun.arena;
    auto succ = base[pred->outgoing[edge]];
    auto predOffset = pred - base;

    auto split = new (arena) LowerBlock(pred->fun, StringId(), BlockIndex(fun.blocks.size()));
    fun.blocks.push(arena, split - base);

    // Wired up by hand rather than through addInst, which would append the split block to `succ`'s
    // incoming list instead of replacing the predecessor entry that the phis still refer to.
    auto jmp = (LowerInst*)new (arena) LowerInstJmp(succ - base);
    jmp->block = split - base;
    split->terminator = jmp - base;
    split->outgoing[0] = succ - base;
    split->incoming.push(arena, predOffset);

    auto je = (LowerInstJe*)base[pred->terminator];
    assertTrue(je->kind == LowerInst::Je);
    if(edge == 0) je->then = split - base;
    else je->otherwise = split - base;
    pred->outgoing[edge] = split - base;

    for(Size i = 0; i < succ->incoming.size(); i++) {
        if(succ->incoming.get(base, i) == predOffset) {
            succ->incoming.set(base, i, split - base);
            break;
        }
    }

    for(auto p: succ->phis.contents(base)) {
        auto sources = base[p]->sources();
        for(Size i = 0; i < sources.size(); i++) {
            if(sources.ptr[i] == predOffset) sources.ptr[i] = split - base;
        }
    }
}

/*
 * Which edges are split, and why it is every critical one rather than only the ones a phi transfer
 * needs.
 *
 * A phi transfer needs an insertion point on its edge, and so does a *location change* - a web that
 * is in a register inside a loop and in its home outside it has to be carried across every edge of
 * the boundary (§5.10 of place.cpp). The two are the same requirement, and a critical edge - a
 * branching predecessor into a joining successor - is exactly the shape that has no such point:
 * a copy at the end of the predecessor runs on the way to both successors, and one at the head of the
 * successor runs on the way in from all of them.
 *
 * Splitting only the phi edges left the second half unserved, and the measurement is what says how
 * much: **193 of `Matrix`'s 257 region candidates were refused for want of an insertion point**, which
 * is three quarters of everything that survived every other test. A loop's exit edge is critical
 * almost by construction - the block after a loop joins the path that ran it with the path that
 * skipped it.
 *
 * What it costs is a block per critical edge, and the answer to that is already here: §3.2.3 emits
 * nothing for a block whose whole content is a jump, so an edge nothing lands on costs no byte and no
 * label. What is left is that the *layout* sees the extra blocks, which is measured rather than
 * assumed - see §49 of test/bench/findings.md.
 */
void splitPhiEdges(LowerBase base, LowerFunction& fun) {
    // Snapshotted because splitting appends to the block list, and a freshly created split block
    // has a single successor and so can never itself need splitting.
    SmallArray<LowerPtr<LowerBlock>, 64> original;
    for(auto b: fun.blocks.contents(base)) original.push(b);

    for(auto offset: original) {
        auto pred = base[offset];

        // Only a block with two successors can reach a successor on a path it might not take.
        if(!pred->outgoing[0] || !pred->outgoing[1]) continue;

        for(Size edge = 0; edge < 2; edge++) {
            auto succ = base[pred->outgoing[edge]];

            // A successor with one predecessor already has an insertion point of its own: the head of
            // the block, which only this edge reaches. Splitting there would add a jump for nothing.
            // Both arms reaching one block counts as two predecessors, which `incoming` records twice.
            if(succ->phis.isEmpty() && succ->incoming.size() < 2) continue;

            splitEdge(base, fun, pred, edge);
        }
    }
}


// Inserts `inst` into `block`'s instruction list at `at`, shifting what follows up one. The list has
// no insert of its own, and the linear shift costs less than adding one would: this runs once per
// stack argument, over a list every pass already walks end to end.
void insertInstAt(LowerBase base, LowerBlock* block, Size at, LowerInst* inst) {
    auto& arena = base[block->fun]->arena;

    inst->block = block - base;
    for(auto use: inst->used()) base[use]->uses.push(arena, inst - base);

    block->instructions.push(arena, inst - base);

    for(auto i = block->instructions.size() - 1; i > at; i--) {
        block->instructions.set(base, i, block->instructions.get(base, i - 1));
    }

    block->instructions.set(base, at, inst - base);
}

// Takes an instruction nothing reads any more out of its block, and with it the uses it contributed.
// Dropping those is what makes the next instruction of a folded address chain dead in turn, so the
// whole chain comes out by removing its instructions in order.
void removeInst(LowerBase base, LowerInst* inst) {
    for(auto offset: inst->used()) {
        auto v = base[offset];
        auto uses = v->uses.contents(base);

        for(Size i = 0; i < uses.size(); i++) {
            if(base[uses[i]] == inst) { v->uses.remove(base, i); break; }
        }
    }

    auto block = base[inst->block];
    auto list = block->instructions.contents(base);

    for(Size i = 0; i < list.size(); i++) {
        if(base[list[i]] == inst) {
            block->instructions.remove(base, i);
            return;
        }
    }

    assertTrue("removing an instruction that is not in its own block" == nullptr);
}

// Moves `user`'s use of `from` over to `to`. Both use lists have to reflect it: they are how every
// later pass finds who consumes a value, and a stale entry would keep a dead value looking live.
void replaceUse(LowerBase base, LowerValue* from, LowerInst* user, LowerValue* to) {
    auto uses = from->uses.contents(base);

    for(Size i = 0; i < uses.size(); i++) {
        if(base[uses[i]] == user) {
            from->uses.remove(base, i);
            break;
        }
    }

    to->uses.push(base[base[user->block]->fun]->arena, user - base);
}

// The same for every reader at once, which is what replacing a value with an equivalent one takes.
// The user list is snapshotted because moving a use rewrites the very list it is read from, and a
// user that reads the value twice appears twice and moves both of its entries across.
void replaceAllUses(LowerBase base, LowerValue* from, LowerValue* to) {
    InstChain users;
    for(auto u: from->uses.contents(base)) users.push(base[u]);

    for(auto user: users) {
        replaceUse(base, from, user, to);

        auto used = user->used();
        for(Size i = 0; i < used.size(); i++) {
            if(base[used[i]] == from) used[i] = to - base;
        }
    }
}

/*
 * Where a constant an expansion needs everywhere goes, which is the entry block's *successor* rather
 * than the entry block itself.
 *
 * `LowerFunction`'s entry block is implicit and holds no instructions - its terminator is index
 * zero, which is what lets the legalizer emit the incoming argument copies ahead of everything the
 * function executes (`runLegalizer` asserts it). So the first block that may hold one is the one that
 * block jumps to, and a value defined at the top of it dominates every use for the same reason the
 * entry block does: every path through the function goes through it.
 *
 * Why bother, rather than building the constant beside its reader: a pooled constant built beside
 * one is the load *immediately above* it, and `tryFoldLoad` takes the load immediately above - so
 * the constant wins the addressing mode and the value being operated on has to be loaded into a
 * register first. Two instructions and two loads per iteration where there should be one of each,
 * and the pooled one is re-read from `.rodata` every time round. See `expandVectorAbs`, which is
 * where that was measured. What it costs is a register held across the function, and it is the
 * cheapest kind: a load of a global nothing writes is rematerializable (`recipeFor` in place.cpp),
 * so a function under pressure spills it by forgetting it.
 */
/*
 * Where an instruction sits in its own block, or nothing if it is not in one.
 *
 * A linear scan, and it is asked once per rewrite rather than once per instruction - the callers are
 * expansions placing work *beside a value they were handed* rather than beside themselves, which is
 * a question only about the handful of constants a function has.
 */
Maybe<Size> positionOf(LowerBase base, LowerBlock* block, LowerInst* inst) {
    if(!block) return Nothing();

    for(Size at = 0; at < block->instructions.size(); at++) {
        if(base[block->instructions.get(base, at)] == inst) return Just(at);
    }

    return Nothing();
}

LowerBlock* constantHome(LowerBase base, LowerFunction& fun) {
    if(fun.blocks.isEmpty()) return nullptr;

    auto entry = base[fun.blocks.get(base, 0)];
    auto terminator = base[entry->terminator];
    if(terminator->kind != LowerInst::Jmp) return nullptr;

    return base[((LowerInstJmp*)terminator)->then];
}

// Moves an instruction to just above `into`'s terminator. Only the lists change: every value it
// reads keeps its use and its own result keeps its readers, so nothing outside the two blocks has to
// be told - which is what makes this a placement change rather than a rewrite.
void moveInstToEndOf(LowerBase base, LowerInst* inst, LowerBlock* into) {
    auto from = base[inst->block];
    auto list = from->instructions.contents(base);

    for(Size i = 0; i < list.size(); i++) {
        if(base[list[i]] == inst) { from->instructions.remove(base, i); break; }
    }

    inst->block = into - base;
    into->instructions.push(base[into->fun]->arena, inst - base);
}
