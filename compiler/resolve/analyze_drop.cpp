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
 *
 * ---------------------------------------------------------------------------------------------
 * Drop flags.
 *
 * Both rules meet `OwnState::Maybe`, and it is the one answer neither of them can act on: the slot
 * owns something on some paths here and nothing on others, so whether the teardown runs is a
 * question about *this execution* rather than about the program. The standard answer is a run-time
 * bit, and this is it.
 *
 * **The flag is the lattice made into a value.** One `Bool` local per root that needs one, written
 * at exactly the points `transferState` changes the state it stands for - an allocation empties the
 * slot, an init fills it, a move empties it again - so at every program point the flag holds `1`
 * exactly when the lattice says `Owned`. That equivalence is the whole correctness argument, and it
 * is why the writes are placed by mirroring that function rather than by a rule of their own.
 *
 * **The conditional drop is a branch, not a field.** `InstDrop` could have carried the flag and left
 * each backend to build the test around it, and it used to be designed that way. What it becomes
 * instead is `je %flag, guarded, tail` around an ordinary unconditional drop - which is the same
 * machinery `insertEdgeDrops` already needed, and which every pass after this one already
 * understands. The optimizer is the reason it is worth the blocks: a flag whose writes all agree is
 * forwarded to a constant by opt_promote, the branch folds, and the drop either becomes
 * unconditional or disappears - none of which a pass that has to treat a flag as opaque can do.
 *
 * **Handing the storage back is not part of the question.** A drop of a heap-placed local does three
 * things, and only two of them are about the value: `drop` runs the effect and `reclaim` releases
 * what the value's members hold, both of which belong to whoever owns the value now - but
 * `releaseStorage` hands back the allocation *this frame* made, and a move takes the contents out of
 * that allocation rather than taking the allocation. So a flagged drop that releases storage is
 * split in two: the guarded half runs the teardown, and the release lands after the join and runs
 * whichever way the branch went. Leaving it inside the guard is a leak on exactly the path the flag
 * exists to describe, and it is not one any counter in a fixture can see.
 *
 * **Nothing resets the flag after a drop**, and it does not have to. A drop is placed where the
 * value's liveness ends, so for the flag to be read again on the same path something must reach it
 * afterwards: another last-use drop needs the local live there, which contradicts the first one's
 * placement; an overwrite drop needs an `Assign` to the slot, which records an *overwrite* and so
 * keeps the local live back through the first drop's position, contradicting it again; and any
 * other route runs through an init or a move, both of which write the flag. What is left is a flag
 * left holding `1` past a value nothing can name, which nothing reads.
 */

// One drop that belongs on a CFG edge rather than inside a block - the branch case, where a value
// is live down one arm and dead down the other.
struct EdgeDrop {
    U32 local = 0;
    Size fromBlock = 0;
    Size toBlock = 0;

    // Whether the teardown is the flag's question rather than this pass's - see PendingDrop.
    bool conditional = false;
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
    auto phis = block->phiCount();

    if(index <= range.first + phis) return 0;

    auto position = Size(index - range.first - phis);
    return min(position, block->instructionCount());
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

            /*
             * A `->` parameter the body never mentions, which is owned before the first instruction
             * runs and so is dead everywhere.
             *
             * Every other owned slot becomes owned at an `init` this walk can see, and the rule
             * below - live before, dead after - places its teardown at the last thing that read it.
             * A sunk parameter has no such point: the handover happened at the call site, so a body
             * that never names it leaves nothing for "dead after" to be after, and what the callee
             * was given was quietly leaked. `fn f(->h: Held) -> Int = 0` is the whole of it.
             *
             * At the top of the entry block rather than at the end of it. There may be no other
             * instruction to sit before - a body that is one `ret` is exactly the shape this misses -
             * and a drop placed after a terminator is a drop that never runs.
             */
            if(b == 0 && !liveBefore && analysis.ownedOnEntry(U32(l))) {
                blockDrops[b].push(PendingDrop { U32(l), U32(range.first), nullptr, false });
                continue;
            }

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
                    blockDrops[b].push(PendingDrop { U32(l), U32(i + 1), nullptr, maybeAfter });
                }

                liveBefore = liveAfter;
            }
        }

        // The branch case: live down one arm and dead down the other. liveOut is the union over
        // successors, so this can only arise where a block has more than one - which is why there
        // is no corresponding "drop at the end of the block" case.
        for(auto successor: block->successors()) {
            if(!successor) continue;

            auto successorIndex = analysis.local[successor]->index;

            for(Size l = 0; l < count; l++) {
                if(!analysis.tracked[l].owned || !analysis.tracked[l].droppable) continue;
                if(!analysis.liveOut[b][l] || analysis.liveIn[successorIndex][l]) continue;

                auto state = range.end > range.first
                    ? analysis.stateBefore[range.end - 1][l] : OwnState::Uninitialized;

                // The terminator itself never changes ownership, so the state before it is the
                // state on the edge.
                if(state == OwnState::Owned || state == OwnState::Maybe) {
                    edgeDrops.push(EdgeDrop { U32(l), b, successorIndex, state == OwnState::Maybe });
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
                    // The slot held something on some of the paths here, so what this write owes is
                    // the same drop under the flag that says which - see elaborateFlaggedDrops. The
                    // place is still the write's own, which is what keeps `v.f = x` releasing `v.f`.
                    blockDrops[b].push(PendingDrop { root, U32(i), pointer, true });
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
 * The flags, and where they are written.
 */

/*
 * Which flag guards which local's teardown.
 *
 * An association list rather than one entry per local, which is the one place this pass departs from
 * how everything around it is keyed. The measurement is the argument: 93% of the functions in the
 * corpus have four locals or fewer and the widest has 45, so a per-local row is small either way -
 * but the number of locals that carry a *flag* is at most a handful and is zero in every body but
 * the few that conditionally move something. So the dense form is a table that is almost always
 * entirely `maxLimit`, and the sparse one is four entries that never reach the heap.
 *
 * It is also why this is a local of insertDrops rather than a row in AnalysisScratch. A per-local
 * table there would be sized to the widest function in the program and allocate once for it -
 * which is exactly what `order`, `tracked` and `demand` are and why those are plain arrays - and
 * inline storage cannot help something whose whole point is to grow to the widest case. Four
 * entries inline can, because four is the answer for every function rather than a guess about one.
 */
struct FlagFor {
    U32 local = 0;
    U32 flag = 0;
};

using FlagMap = SmallArray<FlagFor, 4>;

// The flag guarding this local, or maxLimit where it has none. Linear because the list is four
// entries: a body with more conditional drops than that is not one this shape decides anything
// about, and a body with none does not reach here at all.
static U32 flagFor(const FlagMap& flags, U32 local) {
    for(auto& entry: flags) {
        if(entry.local == local) return entry.flag;
    }

    return maxLimit<U32>;
}

/*
 * One flag: a `Bool` slot of this frame's, and the allocation that makes it one.
 *
 * The allocation is created rather than appended, because it has to end up at the *top* of the entry
 * block and everything else this pass adds is spliced into position by insertBlockDrops. So it
 * travels as far as that splice on the write that declares it - see PendingFlag::allocation.
 */
static U32 makeFlag(Analysis& analysis, ModulePtr<Inst>& allocation) {
    auto& module = analysis.module;
    auto& function = analysis.function;
    auto entry = analysis.blockAt(0);
    auto type = module.scalar.bool_;

    auto created = createInst<InstAlloc>(module, function, *entry, entry->source, StringId(), type,
                                         maxLimit<U32>);

    auto value = (ModulePtr<Value>)((Value*)created - analysis.local);
    created->local = function.addLocal(module, type, StringId(), value, ast::BindType::Ref);

    allocation = (ModulePtr<Inst>)((Inst*)created - analysis.local);
    return created->local;
}

/*
 * A flag for every local a conditional drop names, and a write of it wherever its state changes.
 *
 * The writes mirror transferState exactly, in the same order: a move empties the slot, an
 * allocation empties it again for the next time round a loop, and an init fills it. Where one
 * instruction does two of those the last one written wins, which is the same "moves before inits"
 * the lattice states - `x = consume(x)` empties the slot and fills it again, and what survives is
 * the second.
 *
 * The declaring write is in the entry block and holds whether the slot arrives owned, which is a
 * question with exactly one answer: a `->` parameter's storage comes filled and everything else
 * starts empty. It is also what makes every read of a flag reachable-from-a-write, which is the
 * condition opt_promote forwards under.
 */
static void assignFlags(Analysis& analysis, DropList& blockDrops, EdgeDropList& edgeDrops,
                        FlagMap& flags, FlagList& blockFlags) {
    auto count = analysis.localCount;
    flags.clear();

    auto require = [&](U32 local) {
        if(local >= count || flagFor(flags, local) != maxLimit<U32>) return;

        ModulePtr<Inst> allocation = nullptr;
        auto flag = makeFlag(analysis, allocation);
        flags.push(FlagFor { local, flag });

        blockFlags[0].push(PendingFlag {
            flag, U32(analysis.blockRanges[0].first), isParameterSlot(analysis, local), allocation
        });
    };

    for(Size b = 0; b < analysis.blockCount(); b++) {
        for(auto& pending: blockDrops[b]) {
            if(pending.conditional) require(pending.local);
        }
    }

    for(auto& edge: edgeDrops) {
        if(edge.conditional) require(edge.local);
    }

    // Nothing below has anything to write about, which is every body but the few that conditionally
    // move something - so the walk over every instruction of every function is skipped here rather
    // than being entered and finding nothing.
    if(flags.isEmpty()) return;

    for(Size b = 0; b < analysis.blockCount(); b++) {
        auto range = analysis.blockRanges[b];

        for(Size i = range.first; i < range.end; i++) {
            auto& effects = analysis.effects[i];
            auto& instruction = *analysis.local[analysis.order[i]];

            auto write = [&](U32 local, bool value) {
                auto flag = flagFor(flags, local);
                if(flag == maxLimit<U32>) return;

                blockFlags[b].push(PendingFlag { flag, U32(i + 1), value });
            };

            for(auto moved: effects.moves) write(moved, false);
            if(instruction.kind == Value::Alloc) {
                for(auto def: effects.defs) write(def, false);
            }

            for(auto init: effects.inits) write(init, true);
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
    while(value->kind == Value::Cast || value->kind == Value::Bitcast) {
        value = analysis.local[((InstUnary*)value)->from];
    }

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

    auto drop = createInst<InstDrop>(module, analysis.function, block, source, StringId(),
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

    auto drop = createInst<InstDrop>(module, analysis.function, block, source, StringId(),
                                     module.scalar.unit, place, ownership.drop, ownership.reclaim);

    drop->drop = drop_;
    drop->reclaim = reclaim;
    return drop;
}

/*
 * One flag write, as the two instructions it is: the storage where this is the declaring one, and
 * the write itself.
 */
static void makeFlagWrite(Analysis& analysis, Block& block, const PendingFlag& pending,
                          LocationId source, SmallArray<ModulePtr<Inst>, 4>& into) {
    auto& module = analysis.module;
    auto& function = analysis.function;

    if(pending.allocation) into.push(pending.allocation);

    auto value = addConstant<ConstInt>(module, function, block, source, module.scalar.bool_,
                                       pending.value ? 1 : 0);

    auto write = createInst<InstInit>(module, function, block, source, StringId(), module.scalar.unit,
                                      Place::inLocal(pending.flag),
                                      (ModulePtr<Value>)((Value*)value - analysis.local),
                                      pending.allocation ? Value::Init : Value::Assign);

    into.push((ModulePtr<Inst>)((Inst*)write - analysis.local));
}

// One conditional drop as it stands in the body once it has been spliced in, and the flag that
// decides whether it runs. Collected here because the block it sits in is about to be cut in two
// around it, which is a rewrite of a different shape - see elaborateFlaggedDrops.
struct FlaggedDrop {
    ModulePtr<Inst> drop = nullptr;
    U32 flag = 0;
};

using FlaggedDropList = SmallArray<FlaggedDrop, 8>;

static void insertBlockDrops(Analysis& analysis, DropList& blockDrops, FlagList& blockFlags,
                             FlagMap& flags, FlaggedDropList& flagged) {
    for(Size b = 0; b < analysis.blockCount(); b++) {
        if(blockDrops[b].isEmpty() && blockFlags[b].isEmpty()) continue;

        IrEditor editor(analysis.module, analysis.function);

        auto block = analysis.blockAt(b);
        SmallArray<ModulePtr<Inst>, 8> existing;
        for(auto instruction: block->instructions(analysis.local)) existing.push(instruction);

        // Positions are computed against the original numbering, so they are resolved before
        // anything is inserted and applied in one pass afterwards. The phase is the tie-break at one
        // position: a flag write records what the instruction above it did, so it belongs in front
        // of a drop that the same instruction's post-state asked for.
        SmallArray<Size, 8> positions;
        SmallArray<U8, 8> phases;
        SmallArray<ModulePtr<Inst>, 8> instructions;

        auto sourceAt = [&](U32 before) {
            return analysis.local[analysis.order[min(Size(before), analysis.instructionCount - 1)]]->source;
        };

        for(auto& pending: blockFlags[b]) {
            SmallArray<ModulePtr<Inst>, 4> built;
            makeFlagWrite(analysis, *block, pending, sourceAt(pending.before), built);

            for(auto instruction: built) {
                positions.push(positionInBlock(analysis, b, pending.before));
                phases.push(0);
                instructions.push((ModulePtr<Inst>)(editor.append(*block, analysis.local[instruction]) - analysis.local));
            }
        }

        for(auto& pending: blockDrops[b]) {
            auto position = positionInBlock(analysis, b, pending.before);
            auto source = sourceAt(pending.before);

            auto drop = pending.overwrite
                ? makeOverwriteDrop(analysis, *block, pending.overwrite, source)
                : makeDrop(analysis, *block, pending.local, source);

            if(!drop) continue;

            positions.push(position);
            phases.push(1);

            // Appended rather than spliced, because appending is what records the uses a drop owes -
            // a drop that named a local and was in no use list is what made a whole-function use
            // rebuild necessary in compiler/opt. The list is put into the order the positions ask
            // for below, which is a permutation and so costs nothing.
            auto inserted = (ModulePtr<Inst>)(editor.append(*block, drop) - analysis.local);
            instructions.push(inserted);

            // Read through `flags` rather than trusted from `conditional`, so that a root with no
            // row in the state table - a write through a borrow or into a global, neither of which
            // has one - stays the unconditional drop it was placed as.
            auto flag = flagFor(flags, pending.local);
            if(pending.conditional && flag != maxLimit<U32>) flagged.push(FlaggedDrop { inserted, flag });
        }

        SmallArray<ModulePtr<Inst>, 16> ordered;
        for(Size i = 0; i <= existing.size(); i++) {
            for(U8 phase = 0; phase < 2; phase++) {
                for(Size d = 0; d < positions.size(); d++) {
                    if(positions[d] == i && phases[d] == phase) ordered.push(instructions[d]);
                }
            }

            if(i < existing.size()) ordered.push(existing[i]);
        }

        editor.reorder(*block, Buffer<ModulePtr<Inst>>(ordered.pointer(), ordered.size()));
    }
}

/*
 * Splitting an edge to carry its drops.
 *
 * The alternative would be to put the drop at the top of the successor, which is only correct when
 * every path into it agreed - and the case this exists for is precisely the one where they do not.
 * Everything that names the old edge has to be redirected, which is `IrEditor::splitEdge`'s job:
 * the branch arm, both block graphs, and the alternative each phi in the successor reads over it.
 *
 * Per *arm* rather than per successor, because `je %c, X, X` is two edges into one block and each
 * of them owes these drops. It cannot arise today and the loop is still written this way: the drop
 * only exists where a value is live down one arm and dead down the other, and two arms at one block
 * make `liveOut` the union of one thing with itself. Writing it as "the edge to X" would be one
 * unstated assumption standing between that argument and a wrong answer.
 */
static void splitEdge(Analysis& analysis, Size fromIndex, Size toIndex, EdgeDropList& drops,
                      FlagMap& flags, FlaggedDropList& flagged) {
    auto base = analysis.local;
    auto from = analysis.blockAt(fromIndex);
    auto toPointer = analysis.function.blocks.get(base, toIndex);

    IrEditor editor(analysis.module, analysis.function);

    for(Size successor = 0; successor < kMaxSuccessors; successor++) {
        if(from->successor(successor) != toPointer) continue;

        auto split = editor.splitEdge(*from, successor);

        for(auto& edge: drops) {
            auto drop = makeDrop(analysis, *split, edge.local, split->source);
            if(!drop) continue;

            auto inserted = (ModulePtr<Inst>)(editor.append(*split, drop) - analysis.local);
            auto flag = flagFor(flags, edge.local);

            if(edge.conditional && flag != maxLimit<U32>) flagged.push(FlaggedDrop { inserted, flag });
        }
    }
}

static void insertEdgeDrops(Analysis& analysis, EdgeDropList& edgeDrops, FlagMap& flags,
                            FlaggedDropList& flagged) {
    // Grouped per edge, so one split block carries every drop that edge owes rather than one per.
    while(edgeDrops.size()) {
        auto first = edgeDrops[0];
        EdgeDropList here;
        EdgeDropList remaining;

        for(auto& drop: edgeDrops) {
            if(drop.fromBlock == first.fromBlock && drop.toBlock == first.toBlock) here.push(drop);
            else remaining.push(drop);
        }

        splitEdge(analysis, first.fromBlock, first.toBlock, here, flags, flagged);

        // Replaced rather than assigned - see SmallArray. Assigning one of these appends, and a
        // worklist that never shrinks is a loop that never ends.
        replaceContents(edgeDrops, remaining);
    }
}

/*
 * The branch a flagged drop stands for.
 *
 * Three blocks out of one: what was in front of the drop, the drop on its own, and everything that
 * followed. The flag is read where the split happened and the test is `je`, so the guarded block is
 * reached exactly when the slot still owns something.
 *
 * **The block list is repaired here rather than re-derived.** `Function::blocks` has to stay in
 * reverse postorder - resolve/lower.cpp walks it in list order and asserts every operand it meets
 * has already been lowered - and both `addBlock` and `splitBlock` put the new block at the end,
 * which for the tail half of a cut block is exactly wrong. Splicing the two new blocks in behind the
 * one they came from is that order restored *and* nothing else moved: a body whose blocks were in
 * RPO before is in RPO after, with three where there was one. Recomputing the order globally would
 * reach the same conclusion and reorder blocks this pass never touched.
 *
 * The list is written once at the end, because a drop in the tail half of one cut is elaborated
 * against the block it is in now - which is the tail, and which is already in the order.
 */
static void elaborateFlaggedDrops(Analysis& analysis, FlaggedDropList& flagged) {
    if(flagged.isEmpty()) return;

    auto& module = analysis.module;
    auto& function = analysis.function;
    auto base = analysis.local;

    Array<ModulePtr<Block>> order;
    for(auto pointer: function.blocks.contents(base)) order.push(pointer);

    for(auto& entry: flagged) {
        IrEditor editor(module, function);

        auto pointer = entry.drop;
        auto& dropped = (InstDrop&)*base[pointer];
        auto block = base[base[pointer]->block];
        auto blockPointer = (ModulePtr<Block>)((Block*)block - base);
        auto source = base[pointer]->source;

        // A drop whose only job is handing the allocation back has nothing conditional about it -
        // see the header. Left where it is rather than wrapped in a branch that always runs.
        if(!dropped.drop && !dropped.reclaim) continue;

        Size index = 0;
        for(Size i = 0; i < block->instructionCount(); i++) {
            if(block->instructionAt(base, i) == pointer) index = i;
        }

        // The cut leaves `block` without a terminator, which is what lets the test below be appended
        // as an ordinary instruction rather than replacing one.
        auto tail = editor.splitBlock(*block, index);
        auto tailPointer = (ModulePtr<Block>)((Block*)tail - base);

        auto guarded = function.addBlock(module);
        auto guardedPointer = (ModulePtr<Block>)((Block*)guarded - base);
        guarded->source = source;

        editor.moveInstruction(pointer, *guarded);
        editor.append(*guarded, createInst<InstJmp>(module, function, *guarded, source, StringId(),
                                                    module.scalar.unit, tailPointer));

        auto read = createInst<InstLoadPlace>(module, function, *block, source, StringId(),
                                              module.scalar.bool_, Place::inLocal(entry.flag));
        editor.append(*block, read);

        editor.append(*block, createInst<InstJe>(module, function, *block, source, StringId(),
                                                 module.scalar.unit,
                                                 (ModulePtr<Value>)((Value*)read - base),
                                                 guardedPointer, tailPointer));

        /*
         * The release, taken out of the guard and put after the join.
         *
         * `releaseStorage` is this frame's allocation rather than the value's, so a move that took
         * the contents out of it left the allocation exactly where it was - and skipping the free
         * because the teardown was skipped is a leak on that path. The tail is where both arms meet,
         * which is the first position that runs whichever way the flag went.
         */
        if(dropped.releaseStorage) {
            dropped.releaseStorage = false;

            auto release = createInst<InstDrop>(module, function, *tail, source, StringId(),
                                                module.scalar.unit, dropped.place,
                                                dropped.dropKind, dropped.reclaimKind);

            release->releaseStorage = true;
            editor.append(*tail, release);

            // At the top of the tail rather than at the end of it, which is where appending put it.
            // The list is otherwise untouched, so this is a permutation - see IrEditor::reorder.
            SmallArray<ModulePtr<Inst>, 16> ordered;
            ordered.push((ModulePtr<Inst>)((Inst*)release - base));

            for(auto instruction: tail->instructions(base)) {
                if(instruction != (ModulePtr<Inst>)((Inst*)release - base)) ordered.push(instruction);
            }

            editor.reorder(*tail, Buffer<ModulePtr<Inst>>(ordered.pointer(), ordered.size()));
        }

        // Behind the block the cut came out of, in place. Both new blocks were appended by the
        // editor, so what this does is take them off the end and put them where reverse postorder
        // wants them - which is immediately after their predecessor, since that is where the one
        // block they replaced was.
        for(Size i = order.size(); i-- > 0;) {
            if(order[i] == guardedPointer || order[i] == tailPointer) order.remove(i);
        }

        for(Size i = 0; i < order.size(); i++) {
            if(order[i] != blockPointer) continue;

            order.insert(i + 1, guardedPointer);
            order.insert(i + 2, tailPointer);
            break;
        }
    }

    IrEditor(module, function).setBlockOrder(Buffer<ModulePtr<Block>>(order.pointer(), order.size()));
}

/*
 * Both rules and the rewrite, which is the only order they are ever wanted in.
 *
 * Nothing is inserted once something has been reported: a body that failed a check is one whose
 * drops would be derived from a lifetime the pass could not settle.
 *
 * The four steps after the rules are one sequence rather than four passes, and the order is what
 * each needs from the one before it. The flags have to exist before anything is spliced in, because
 * a flag write is spliced in with the drops and by the same positions; the drops have to be in their
 * blocks before those blocks are cut, because the cut is stated over the instruction; and the cut
 * comes last because it is the only step that invalidates the numbering everything above indexes by.
 */
void insertDrops(Analysis& analysis) {
    // One row per block, each holding the few drops that block ends up needing. The scratch's, so
    // a body analysed after another writes into the rows that one grew - see AnalysisScratch.
    auto& blockDrops = analysis.scratch.blockDrops;
    auto& blockFlags = analysis.scratch.blockFlags;
    blockDrops.reset(analysis.blockCount());
    blockFlags.reset(analysis.blockCount());

    EdgeDropList edgeDrops;
    placeDrops(analysis, blockDrops, edgeDrops);
    placeOverwriteDrops(analysis, blockDrops);
    if(!analysis.ok) return;

    FlagMap flags;
    FlaggedDropList flagged;

    assignFlags(analysis, blockDrops, edgeDrops, flags, blockFlags);
    insertBlockDrops(analysis, blockDrops, blockFlags, flags, flagged);
    insertEdgeDrops(analysis, edgeDrops, flags, flagged);
    elaborateFlaggedDrops(analysis, flagged);
}
