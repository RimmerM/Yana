#include "analyze_pass.h"

/*
 * Where the drops go, and the rewrite that puts them there.
 *
 * Two rules decide, and each has a pass of its own because they are about different things - see
 * placeDrops and placeOverwriteDrops. Everything after them is the rewrite: what a drop of one local
 * actually costs, and the block and edge surgery that gets the instructions into the body.
 *
 * Nothing here decides a *fact*. Both rules read liveness and the ownership lattice and neither
 * computes anything, which is what makes a misplaced drop localizable: it is either this file's
 * reading of those facts or the facts themselves.
 */

// One drop that belongs on a CFG edge rather than inside a block - the branch case, where a value
// is live down one arm and dead down the other.
struct EdgeDrop {
    U32 local = 0;
    Size fromBlock = 0;
    Size toBlock = 0;
};

// The drops each block needs, and the ones that belong on an edge rather than in a block. Both are
// short in every body that has any at all - a function that drops eight things on one edge is not
// one this bound decides anything about.
using EdgeDropList = SmallArray<EdgeDrop, 8>;

/*
 * The first rule: a value's lifetime ends where nothing reaches it any more, which is a fact about
 * a local and is what this walks liveness for.
 */

// Where in a block's instruction list a linear index sits. A drop that would land on the
// terminator goes at the end of the list instead, which is the same position - the terminator is
// held apart from the list rather than in it.
static Size positionInBlock(Analysis& analysis, Size blockIndex, U32 index) {
    auto block = analysis.blockAt(blockIndex);
    auto range = analysis.blockRanges[blockIndex];
    auto phis = block->phis.size();

    if(index <= range.first + phis) return 0;

    auto position = Size(index - range.first - phis);
    return min(position, block->instructions.size());
}

static void placeDrops(Analysis& analysis, DropList& blockDrops, EdgeDropList& edgeDrops) {
    auto count = analysis.localCount;

    // Both are the program's buffers rather than this block's: the rows below are re-sized per
    // block and the set is cleared per block, so a function with a hundred of them allocates for
    // the longest one rather than for each. See AnalysisScratch.
    auto& after = analysis.scratch.blockLiveness;
    auto& live = analysis.scratch.work;

    for(Size b = 0; b < analysis.blockCount(); b++) {
        auto block = analysis.blockAt(b);
        auto range = analysis.blockRanges[b];
        if(range.end == range.first) continue;

        // Liveness at each point inside the block, derived by replaying the backward walk. `after`
        // is the state over the gap following each instruction, which is where a drop goes.
        after.reset(range.end - range.first, count);
        live.copyFrom(analysis.liveOut[b]);

        for(Size i = range.end; i > range.first; i--) {
            after[range.end - i].copyFrom(live);

            auto& effects = analysis.effects[i - 1];
            for(auto def: effects.defs) live.set(def, false);
            for(auto use: effects.uses) live.set(use, true);
            for(auto overwritten: effects.overwrites) live.set(overwritten, true);
        }

        for(Size l = 0; l < count; l++) {
            if(!analysis.tracked[l].owned || !analysis.tracked[l].droppable) continue;

            auto liveBefore = analysis.liveIn[b][l];

            for(Size i = range.first; i < range.end; i++) {
                auto& effects = analysis.effects[i];
                auto liveAfter = after[range.end - 1 - i][l];

                auto defines = false;
                for(auto init: effects.inits) defines = defines || init == l;

                auto moves = false;
                for(auto move: effects.moves) moves = moves || move == l;

                auto before = analysis.stateBefore[i][l];
                auto ownedAfter = defines || (before == OwnState::Owned && !moves);
                auto maybeAfter = !defines && !moves && before == OwnState::Maybe;

                if((liveBefore || defines) && !liveAfter && (ownedAfter || maybeAfter)) {
                    if(maybeAfter) {
                        report(analysis, "this value was moved out of on only some paths reaching its last use - conditional drops need drop flags, which are not implemented yet"_v,
                               analysis.local[analysis.order[i]]->source);
                    } else {
                        blockDrops[b].push(PendingDrop { U32(l), U32(i + 1) });
                    }
                }

                liveBefore = liveAfter;
            }
        }

        // The branch case: live down one arm and dead down the other. liveOut is the union over
        // successors, so this can only arise where a block has more than one - which is why there
        // is no corresponding "drop at the end of the block" case.
        for(auto successor: block->outgoing) {
            if(!successor) continue;

            auto successorIndex = analysis.local[successor]->index;

            for(Size l = 0; l < count; l++) {
                if(!analysis.tracked[l].owned || !analysis.tracked[l].droppable) continue;
                if(!analysis.liveOut[b][l] || analysis.liveIn[successorIndex][l]) continue;

                auto state = range.end > range.first
                    ? analysis.stateBefore[range.end - 1][l] : OwnState::Uninitialized;

                // The terminator itself never changes ownership, so the state before it is the
                // state on the edge.
                if(state == OwnState::Maybe) {
                    report(analysis, "this value is owned on only some paths reaching this branch - conditional drops need drop flags, which are not implemented yet"_v,
                           analysis.local[block->terminator]->source);
                } else if(state == OwnState::Owned) {
                    edgeDrops.push(EdgeDrop { U32(l), b, successorIndex });
                }
            }
        }
    }
}

/*
 * The other kind of drop: the one an overwrite owes.
 *
 * Overwriting storage releases what it held first - the entire reason Init and Assign are two
 * instructions rather than one - and that obligation is about the *place* being written rather than
 * about the slot it is rooted in. `v.f = x` replaces one field and leaves every other member of `v`
 * exactly where it was, so what it owes is a drop of `v.f`; dropping `v` there would release members
 * nothing overwrote, and dropping nothing at all leaks whatever `f` held. Which is why this is one
 * pass over the writes rather than a case inside the per-local walk above: a field write is not a
 * fact about a local's lifetime at all.
 *
 * It reads the ownership state rather than TrackedLocal::owned, because the two answer different
 * questions. `owned` is "does this frame release the slot when it dies", which a `&` parameter's
 * does not - the caller's storage outlives the call and dropping it at the end would release
 * something the caller still holds. An overwrite is not the end of anything: the program asked for
 * the contents to be replaced, the storage stays, and the old contents have to go somewhere whoever
 * owns the slot. What has to be true is only that something was there, which is what the state says.
 *
 * A field of an owned aggregate is always initialized when the aggregate is, since moving a part of
 * a value out of it is rejected outright - see checkMoves. So the root's state is the field's state
 * too, and no per-field lattice is needed to know that the old field is there to release.
 *
 * A borrow root has no state to read, and does not need one: a borrow refers to an initialized
 * value of its type, always. That is a property of `&` rather than something inferred here, and it
 * is what `xs[i] = v` needs, since the borrow `getMut` hands back is a call result with no place
 * behind it to ask about.
 *
 * It holds because nothing can falsify it. A Borrow instruction records a *use* of its root, so
 * checkMoves rejects taking one of storage that is not owned; a move out of a `&` binding and a
 * partial move are both rejected outright; and a borrow that came from a call is a borrow some
 * other body took under those same rules. What is left is Native, where `borrowMut` turns an
 * address into a borrow and the promise becomes the caller's to keep - which is the same tier as
 * every bounds check in this compiler, and already what `getMut(xs, i)` for an `i` past the end
 * was before any of this.
 *
 * A global gets the same answer for a plainer reason: its initializer is a constant, so it holds a
 * value before the program starts and there is no program point at which it does not.
 *
 * Which leaves the raw pointer, and it stays left. `*p = v` releases nothing, because the memory a
 * pointer names is outside the ownership model by definition and nothing here can say what is in
 * it. That is the unsafety Native is named for rather than a case missing from this pass.
 */
static void placeOverwriteDrops(Analysis& analysis, DropList& blockDrops) {
    for(Size b = 0; b < analysis.blockCount(); b++) {
        auto range = analysis.blockRanges[b];

        for(Size i = range.first; i < range.end; i++) {
            auto pointer = analysis.order[i];
            auto& instruction = *analysis.local[pointer];
            if(instruction.kind != Value::Assign) continue;

            auto& write = (InstInit&)instruction;

            if(write.place.root == PlaceRoot::Pointer) continue;

            // A step through a raw pointer leaves the root behind as surely as a pointer root does
            // - `p.f = x` for `p: %Node` writes into memory whose contents nothing here can speak
            // for. What is left is a path that stays inside the storage the root names, which is
            // what makes the root's answer the whole place's answer.
            auto reachable = true;
            for(auto projection: write.place.projections.contents(analysis.local)) {
                auto inside = projection.kind == ProjectionKind::Field ||
                              projection.kind == ProjectionKind::Downcast;

                reachable = reachable && inside;
            }

            if(!reachable) continue;

            auto type = placeType(analysis.module, analysis.function, write.place);
            if(!needsTeardown(analysis.module, type)) continue;

            // The two roots that are initialized by the time anything can name them. Neither has a
            // row in the state table, and neither needs one - see above.
            if(write.place.root == PlaceRoot::Borrow || write.place.root == PlaceRoot::Global) {
                blockDrops[b].push(PendingDrop { maxLimit<U32>, U32(i), pointer });
                continue;
            }

            // Asked after the roots that have no state rather than before them, so that a local
            // index out of range falls out here as the malformed place it is instead of being read
            // as one of those.
            auto root = rootLocal(analysis, write.place);
            if(root == maxLimit<U32>) continue;

            switch(analysis.stateBefore[i][root]) {
                case OwnState::Owned:
                    blockDrops[b].push(PendingDrop { root, U32(i), pointer });
                    break;
                case OwnState::Maybe:
                    report(analysis, "this assignment overwrites a value that was moved out of on only some paths - conditional drops need drop flags, which are not implemented yet"_v,
                           instruction.source);
                    break;
                default:
                    // Uninitialized or moved out of: there is nothing there to release, and filling
                    // the slot again is what this write is.
                    break;
            }
        }
    }
}

/*
 * Rewriting the body.
 */

/*
 * What tearing down one function value actually costs, where this frame can see.
 *
 * The generic teardown of a function value is written against what a call site knows: a code word
 * and an environment word, either of which may have come from anywhere, so it tests the environment
 * for null, finds the closure header in front of the entry point and calls what the header names.
 * None of that is knowledge a *drop site* lacks when the value was built here - there is one
 * instruction that wrote the environment word, and what it wrote is either nothing or the address of
 * a local whose type this frame knows.
 *
 * So the answer is one of three:
 *
 *   Unknown      the value arrived from somewhere - a parameter, a phi, a call - and the generic
 *                teardown is what it is for. `let f = if c then A else B` is the ordinary shape
 *                here: two lambdas reach one drop, and which of them this is is a run-time fact.
 *   Empty        the environment word is the null constant, so the generic teardown would test it,
 *                take the other branch and return. A lambda that captured nothing is this.
 *   Environment  the environment is a frame local, so tearing the closure down *is* tearing that
 *                local down - the same two halves the header would have named, reached by name.
 *
 * Deliberately restricted to a frame-placed environment. A heap one has its storage to hand back as
 * well, and who does that is bookkeeping this would have to move rather than skip; the header path
 * already gets it right.
 */
struct ClosureTeardown {
    enum Kind: U8 {
        Unknown,
        Empty,
        Environment,
    };

    Kind kind = Unknown;
    U32 local = maxLimit<U32>;
};

static ClosureTeardown closureTeardown(Analysis& analysis, U32 localIndex) {
    ClosureTeardown unknown;

    auto slot = analysis.function.localAt(analysis.local, localIndex);
    if(!slot.type || analysis.global[slot.type]->kind != Type::Fun) return unknown;

    // Storage this frame created, so that every write to it is an instruction in this body. A phi
    // result or a parameter is a function value that arrived already built, and what is in it is
    // exactly what this cannot see.
    if(!slot.value || analysis.local[slot.value]->kind != Value::Alloc) return unknown;
    if(((InstAlloc*)analysis.local[slot.value])->local != localIndex) return unknown;

    // Storage this frame also has to hand back is not this shortcut's to take: the release belongs
    // to this local, and redirecting the drop to another one would drop it.
    if(localIndex < analysis.releasesStorage.size() && analysis.releasesStorage[localIndex]) return unknown;

    ModulePtr<Value> environment = nullptr;
    auto seenCode = false;
    auto seenEnv = false;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        if(instruction.kind != Value::Init && instruction.kind != Value::Assign) continue;

        auto& write = (InstInit&)instruction;
        if(write.place.root != PlaceRoot::Local || write.place.local != localIndex) continue;

        // Anything other than the two field initializations the closure builder emits - an
        // assignment, a write of the whole value, a second write of one word - and what the slot
        // holds is no longer settled by one instruction.
        if(instruction.kind != Value::Init || write.place.projections.size() != 1) return unknown;

        auto projection = write.place.projections.get(analysis.local, 0);
        if(projection.kind != ProjectionKind::Field) return unknown;

        if(projection.index == FunValueLayout::kCode) {
            if(seenCode) return unknown;
            seenCode = true;
            continue;
        }

        if(projection.index != FunValueLayout::kEnv || seenEnv) return unknown;

        seenEnv = true;
        environment = write.value;
    }

    if(!seenEnv || !environment) return unknown;

    auto value = analysis.local[environment];

    // Through the cast, because the resolve IR has no pointer immediate: a null address is the
    // integer reinterpreted, which is what constantBits builds and what `null()` expands to.
    while(value->kind == Value::Cast) value = analysis.local[((InstUnary*)value)->from];

    // No environment at all. That bit pattern is what makeFunValue writes for a lambda that captured
    // nothing, and it is the one case where the generic teardown provably does nothing.
    if(value->kind == Value::ConstInt) {
        return ((ConstInt*)value)->value == 0 ? ClosureTeardown { ClosureTeardown::Empty } : unknown;
    }

    // The address of a local, which is what makes the environment storage this frame can name -
    // and naming it is all a teardown of it needs.
    if(value->kind != Value::Address) return unknown;

    auto& place = ((InstAddress*)value)->place;
    if(place.root != PlaceRoot::Local || place.projections.isNotEmpty()) return unknown;
    if(place.local >= analysis.localCount) return unknown;

    auto envSlot = analysis.function.localAt(analysis.local, place.local);
    if(!envSlot.closureEnv) return unknown;
    if(!envSlot.value || analysis.local[envSlot.value]->kind != Value::Alloc) return unknown;
    if(((InstAlloc*)analysis.local[envSlot.value])->storage != StorageClass::Stack) return unknown;

    return ClosureTeardown { ClosureTeardown::Environment, place.local };
}

static InstDrop* makeDrop(Analysis& analysis, Block& block, U32 localIndex, LocationId source) {
    auto& module = analysis.module;
    auto slot = analysis.function.localAt(analysis.local, localIndex);
    auto ownership = ownershipOf(module, slot.type);

    /*
     * A closure built here is torn down by name rather than through its own header.
     *
     * Nothing about the result differs - the environment is the only thing a function value has to
     * release, and this is the same two halves run on the same storage. What differs is that a
     * closure whose environment holds nothing to tear down now costs no instructions at all, where
     * the generic path costs a load, a test, six instructions of header arithmetic and two indirect
     * calls to a function that returns.
     */
    auto closure = closureTeardown(analysis, localIndex);
    if(closure.kind == ClosureTeardown::Empty) return nullptr;
    if(closure.kind == ClosureTeardown::Environment) {
        return makeDrop(analysis, block, closure.local, source);
    }

    auto drop_ = teardownFor(module, slot.type, Teardown::Drop, source);
    auto reclaim = teardownFor(module, slot.type, Teardown::Reclaim, source);

    // Heap storage this frame owns has to be handed back whether or not the type it holds has
    // anything of its own to run - which is the reclaim half applied to this allocation rather than
    // to its members.
    auto releases = localIndex < analysis.releasesStorage.size() && analysis.releasesStorage[localIndex];
    if(!drop_ && !reclaim && !releases) return nullptr;

    auto drop = createInst<InstDrop>(module, analysis.function, block, source, 0,
                                     module.scalar.unit, Place::inLocal(localIndex),
                                     ownership.drop, ownership.reclaim);

    drop->drop = drop_;
    drop->reclaim = reclaim;
    drop->releaseStorage = releases;

    if(releases && analysis.module.program.freeHeap) {
        analysis.local[analysis.module.program.freeHeap]->used = true;
    }

    return drop;
}

/*
 * The drop a write owes for what it is about to replace.
 *
 * Stated over the write's own place, which is what makes the field case work: `v.f = x` releases
 * `v.f`, and the projections that name it are the ones the write already carries.
 *
 * Nothing here releases storage, unlike makeDrop. Handing an allocation back is what happens when a
 * value's lifetime ends and nothing follows it, and something does follow this one: the write that
 * comes next fills the same storage again. Freeing it here and writing into it afterwards would be
 * a use after free of the frame's own heap slot.
 */
static InstDrop* makeOverwriteDrop(Analysis& analysis, Block& block, ModulePtr<Inst> write,
                                   LocationId source) {
    auto& module = analysis.module;

    // By value: creating the drop may grow the arena, and a reference into it would then be
    // pointing at the old one. The projection list travels as an offset, so the copy names the
    // same projections rather than a second set of them.
    auto place = ((InstInit&)*analysis.local[write]).place;
    auto type = placeType(module, analysis.function, place);
    auto ownership = ownershipOf(module, type);

    auto drop_ = teardownFor(module, type, Teardown::Drop, source);
    auto reclaim = teardownFor(module, type, Teardown::Reclaim, source);
    if(!drop_ && !reclaim) return nullptr;

    auto drop = createInst<InstDrop>(module, analysis.function, block, source, 0,
                                     module.scalar.unit, place, ownership.drop, ownership.reclaim);

    drop->drop = drop_;
    drop->reclaim = reclaim;
    return drop;
}

static void insertBlockDrops(Analysis& analysis, DropList& blockDrops) {
    for(Size b = 0; b < analysis.blockCount(); b++) {
        if(blockDrops[b].isEmpty()) continue;

        auto block = analysis.blockAt(b);
        SmallArray<ModulePtr<Inst>, 8> existing;
        for(auto instruction: block->instructions.contents(analysis.local)) existing.push(instruction);

        // Positions are computed against the original numbering, so they are resolved before
        // anything is inserted and applied in one pass afterwards.
        SmallArray<Size, 8> positions;
        SmallArray<InstDrop*, 8> instructions;

        for(auto& pending: blockDrops[b]) {
            auto position = positionInBlock(analysis, b, pending.before);
            auto source = analysis.local[analysis.order[min(Size(pending.before), analysis.instructionCount - 1)]]->source;

            auto drop = pending.overwrite
                ? makeOverwriteDrop(analysis, *block, pending.overwrite, source)
                : makeDrop(analysis, *block, pending.local, source);

            if(!drop) continue;

            positions.push(position);
            instructions.push(drop);

            // The drop is spliced in below rather than appended, so `Block::add` never sees it and
            // the uses it owes have to be recorded here - see recordInstUses. A drop that named a
            // local and was in no use list is what made `rebuildUses` in compiler/opt necessary.
            recordInstUses(analysis.module, drop);
        }

        block->instructions.clear();
        for(Size i = 0; i <= existing.size(); i++) {
            for(Size d = 0; d < positions.size(); d++) {
                if(positions[d] != i) continue;
                block->instructions.push(analysis.module.arena, (ModulePtr<Inst>)(instructions[d] - analysis.local));
            }

            if(i < existing.size()) block->instructions.push(analysis.module.arena, existing[i]);
        }
    }
}

/*
 * Splitting an edge to carry its drops.
 *
 * The alternative would be to put the drop at the top of the successor, which is only correct when
 * every path into it agreed - and the case this exists for is precisely the one where they do not.
 * Everything that names the old edge has to be redirected: the branch, both block graphs, and any
 * phi in the successor that reads a value from this predecessor.
 */
static void splitEdge(Analysis& analysis, Size fromIndex, Size toIndex, SmallArray<U32, 8>& locals) {
    auto& module = analysis.module;
    auto base = analysis.local;
    auto from = analysis.blockAt(fromIndex);
    auto to = analysis.blockAt(toIndex);

    auto fromPointer = analysis.function.blocks.get(base, fromIndex);
    auto toPointer = analysis.function.blocks.get(base, toIndex);

    auto split = analysis.function.addBlock(module);
    auto splitPointer = split - base;
    split->index = U16(analysis.function.blocks.size() - 1);
    split->source = base[from->terminator]->source;

    for(auto localIndex: locals) {
        auto drop = makeDrop(analysis, *split, localIndex, split->source);
        if(!drop) continue;

        split->instructions.push(module.arena, (ModulePtr<Inst>)(drop - base));
        recordInstUses(module, drop);
    }

    auto jump = createInst<InstJmp>(module, analysis.function, *split, split->source, 0,
                                    module.scalar.unit, toPointer);
    split->terminator = (ModulePtr<Inst>)(jump - base);
    split->outgoing[0] = toPointer;

    // The branch now leaves through the split block instead.
    auto terminator = base[from->terminator];
    if(terminator->kind == Value::Je) {
        auto& branch = (InstJe&)*terminator;
        if(branch.thenBlock == toPointer) branch.thenBlock = splitPointer;
        else if(branch.elseBlock == toPointer) branch.elseBlock = splitPointer;
    } else if(terminator->kind == Value::Jmp) {
        ((InstJmp&)*terminator).target = splitPointer;
    }

    for(auto& outgoing: from->outgoing) {
        if(outgoing == toPointer) outgoing = splitPointer;
    }

    for(Size i = 0; i < to->incoming.size(); i++) {
        if(to->incoming.get(base, i) == fromPointer) to->incoming.set(base, i, splitPointer);
    }

    split->incoming.push(module.arena, fromPointer);

    for(auto phiPointer: to->phis.contents(base)) {
        auto& phi = *base[phiPointer];
        for(Size i = 0; i < phi.inputs.size(); i++) {
            auto input = phi.inputs.get(base, i);
            if(input.block != fromPointer) continue;

            input.block = splitPointer;
            phi.inputs.set(base, i, input);
        }
    }
}

static void insertEdgeDrops(Analysis& analysis, EdgeDropList& edgeDrops) {
    // Grouped per edge, so one split block carries every drop that edge owes rather than one per.
    while(edgeDrops.size()) {
        auto first = edgeDrops[0];
        SmallArray<U32, 8> locals;
        EdgeDropList remaining;

        for(auto& drop: edgeDrops) {
            if(drop.fromBlock == first.fromBlock && drop.toBlock == first.toBlock) locals.push(drop.local);
            else remaining.push(drop);
        }

        splitEdge(analysis, first.fromBlock, first.toBlock, locals);

        // Replaced rather than assigned - see SmallArray. Assigning one of these appends, and a
        // worklist that never shrinks is a loop that never ends.
        replaceContents(edgeDrops, remaining);
    }
}

/*
 * Both rules and the rewrite, which is the only order they are ever wanted in.
 *
 * Nothing is inserted once something has been reported: the two shapes that would need a drop flag
 * report instead of emitting, and a body that got one of those diagnostics is a body whose drops
 * would be derived from a lifetime the pass could not settle.
 */
void insertDrops(Analysis& analysis) {
    // One row per block, each holding the few drops that block ends up needing. The scratch's, so
    // a body analysed after another writes into the rows that one grew - see AnalysisScratch.
    auto& blockDrops = analysis.scratch.blockDrops;
    blockDrops.reset(analysis.blockCount());

    EdgeDropList edgeDrops;
    placeDrops(analysis, blockDrops, edgeDrops);
    placeOverwriteDrops(analysis, blockDrops);
    if(!analysis.ok) return;

    insertBlockDrops(analysis, blockDrops);
    insertEdgeDrops(analysis, edgeDrops);
}
