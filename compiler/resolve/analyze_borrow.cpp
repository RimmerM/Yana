#include "analyze_pass.h"
#include "generic.h"

/*
 * The four checks.
 *
 * Every one of them is stated over the facts the passes before it computed and computes nothing of
 * its own, which is what makes them one file: a diagnostic here is either a rule written wrong or a
 * fact computed wrong, and the two are now in different files.
 *
 * What is deliberately *not* checked is recorded at the end of analyze.cpp.
 */

/*
 * Where one borrow is live, one row per block.
 *
 * **A borrow's extent is a region of the control-flow graph and not an interval of the instruction
 * numbering**, and the difference is the whole reason this struct exists. `numberFunction` lays the
 * blocks out in reverse postorder, which puts a loop's *exit* in front of its *body* whenever the
 * exit edge was the one explored first - so
 *
 *     while i < 4:            -- block 3, numbered [15, 24)
 *         insert(m, i, i)
 *
 *     match find(m, 1):       -- block 2, numbered [9, 15)
 *
 * numbers the lookup *before* the loop that filled the map. Scanning `[borrow, lastUse]` as a range
 * of indices then walks straight through the loop body and reports the `insert` as conflicting with
 * a borrow that is not live anywhere near it - which it did, for every `find` after a fill, until
 * this replaced it.
 *
 * So the extent is computed the way a live range is: backwards from each use to the definition,
 * through predecessors, marking the blocks the value is actually live in. What that costs is a
 * walk per borrow; what it buys is that the answer no longer depends on which arm of a branch the
 * block numbering happened to visit first.
 */
struct BlockExtent {
    // Live across the block's first instruction, and live across its terminator.
    bool liveIn = false;
    bool liveOut = false;

    // The last instruction in this block that uses the borrow, where one does. A flag rather than a
    // sentinel index, because index zero is a real position.
    bool used = false;
    U32 lastUse = 0;
};

struct BorrowExtent {
    // One row per block, indexed by `Block::index` - which is the block's position in
    // `Function::blocks` and therefore also its position in `Analysis::blockRanges`.
    SmallArray<BlockExtent, 16> blocks;

    U32 defBlock = 0;
    U32 defIndex = 0;
};

/*
 * The value is live out of `from`, and so live in to every block on the way back to its definition.
 *
 * The walk stops at the defining block in both senses: it marks that block live-*out* (control did
 * leave it carrying the value) and does not mark it live-in, because above the definition the value
 * does not exist. That is also what makes a definition inside a loop right - a borrow taken each
 * time round is not live across the back edge into its own block.
 *
 * **`stop` is the block defining the value being walked, which is not always the borrow.** A loan is
 * followed through the values that carry it - a call result, a phi - and each of those is defined
 * somewhere of its own; walking back from a use of the *phi* has to stop at the merge, because above
 * it the phi does not exist and the arm that did not run never held the loan. Stopping at the
 * borrow's block instead reports each arm of `if flag then pick(x, y) else pick(y, x)` as
 * conflicting with the other.
 *
 * `visited` is per walk rather than per borrow for the same reason: a block one carrier stopped at
 * is one the next may have to walk through.
 *
 * Iterative rather than recursive, because the depth is the block graph's and this runs once per use
 * of every borrow in every function.
 */
static void markLiveOut(Analysis& analysis, BorrowExtent& extent, U32 stop,
                        SmallArray<U8, 16>& visited, ModulePtr<Block> from) {
    SmallArray<ModulePtr<Block>, 16> pending;
    pending.push(from);

    while(pending.size()) {
        auto block = analysis.local[pending.pop().unwrap()];
        if(block->index >= extent.blocks.size()) continue;
        if(visited[block->index]) continue;
        visited[block->index] = 1;

        extent.blocks[block->index].liveOut = true;
        if(block->index == stop) continue;

        extent.blocks[block->index].liveIn = true;
        for(auto predecessor: block->incoming(analysis.local)) pending.push(predecessor);
    }
}

/*
 * Do two places name storage that can overlap?
 *
 * Same root, and one projection path a prefix of the other. `x.a` and `x.b` are disjoint, `x` and
 * `x.a` are not, and that precision is what makes a `&` parameter usable at all - `moveBy` borrows
 * `p.x` and `p.y` in turn and neither is a conflict with the other.
 *
 * Two deliberate conservatisms. A step through a Deref or an Index leads somewhere this analysis
 * cannot name, so from there on the two are assumed to overlap. And places rooted in raw pointers
 * never conflict with anything, including each other: `%T` carries no aliasing information by
 * construction, and reporting on it would be inventing a rule the language says it does not have.
 */
static bool placesOverlap(ModuleBase base, Place lhs, Place rhs) {
    if(lhs.root != rhs.root) return false;
    if(lhs.root == PlaceRoot::Pointer) return false;
    if(lhs.root == PlaceRoot::Local && lhs.local != rhs.local) return false;
    if(lhs.root == PlaceRoot::Global && lhs.global != rhs.global) return false;

    auto left = lhs.projections;
    auto right = rhs.projections;
    auto leftContents = left.contents(base);
    auto rightContents = right.contents(base);

    auto leftIterator = leftContents.begin();
    auto rightIterator = rightContents.begin();

    for(Size i = 0; i < min(left.size(), right.size()); i++) {
        auto a = *leftIterator;
        auto b = *rightIterator;
        ++leftIterator;
        ++rightIterator;

        if(a.kind == ProjectionKind::Deref || a.kind == ProjectionKind::Index) return true;
        if(b.kind == ProjectionKind::Deref || b.kind == ProjectionKind::Index) return true;
        if(a.kind != b.kind || a.index != b.index) return false;
    }

    // One path ran out, so it is a prefix of the other and names storage containing it.
    return true;
}

/*
 * Exclusivity - Design.md's second question, and the only one of the four with an extent to
 * compute first.
 */

/*
 * How far a borrow's extent reaches.
 *
 * To the last instruction that consumes the borrow value - and then, if one of those was a call
 * that may hand it back, to the last use of what the call produced. That second clause is the
 * whole of Design.md's "the caller conservatively keeps every member borrowed until the last use
 * of all result borrows", and it is why the loan on `objects` does not end at the call to
 * `getMutableEntry` but at the last use of the entry it returned.
 *
 * Transitive, because a caller may hand the result on again: a chain of selectors keeps the
 * original root borrowed for the whole chain. The `seen` list is what makes a value used by two
 * calls, or a loop, terminate instead of walking the graph forever.
 *
 * Which of those uses are reached is one question and *where the value is live on the way to them*
 * is another, and this answers both: every use found below is marked into `extent`, from which the
 * blocks the borrow is live in follow by walking predecessors. See BorrowExtent for why an interval
 * of instruction indices is not an answer to the second.
 */
static void computeExtent(Analysis& analysis, ModulePtr<Inst> pointer, BorrowExtent& extent) {
    extent.blocks.clear();
    for(Size b = 0; b < analysis.blockCount(); b++) extent.blocks.push(BlockExtent());

    auto& borrow = *analysis.local[pointer];
    extent.defBlock = analysis.local[borrow.block]->index;

    auto found = analysis.indexOf.get(U32(pointer));
    extent.defIndex = found ? found.unwrap() : 0;

    ValueList pending;
    ValueList seen;
    pending.push((ModulePtr<Value>)pointer);

    while(pending.size()) {
        auto value = pending.pop().unwrap();

        auto walked = false;
        for(auto& entry: seen) walked = walked || entry == value;
        if(walked) continue;
        seen.push(value);

        /*
         * The block this carrier is defined in, which is what its own uses are walked back to - see
         * markLiveOut. The borrow is one carrier among several and has no special standing here.
         */
        auto carrierBlock = analysis.local[analysis.local[value]->block]->index;

        SmallArray<U8, 16> visited;
        for(Size b = 0; b < extent.blocks.size(); b++) visited.push(0);

        for(auto user: analysis.local[value]->uses(analysis.local)) {
            auto& instruction = *analysis.local[user];

            /*
             * Where this use puts the value.
             *
             * A phi is the one user that does not make the value live in its own block: an operand
             * arrives along one edge, so what it says is that the value is live *out of that
             * predecessor* and nothing about the other arms. Marking the phi's block live-in
             * instead would keep a borrow alive down every path into a merge, which is how a value
             * that a branch discarded ends up conflicting with the branch that discarded it.
             *
             * Every other user makes the value live at its own position, and live in to its block
             * unless that block is where the borrow was taken - in which case it is live from the
             * borrow onwards and no further back.
             */
            if(instruction.kind == Value::Phi) {
                auto& phi = (InstPhi&)instruction;
                for(Size input = 0; input < phi.inputs.size(); input++) {
                    auto arm = phi.inputs.get(analysis.local, input);
                    if(arm.value == value && arm.block) {
                        markLiveOut(analysis, extent, carrierBlock, visited, arm.block);
                    }
                }

                /*
                 * And the phi carries the loan on, which is the other half of the same fact: a merge
                 * of two borrows may name either, so a use of the merged value keeps *both* alive.
                 * Without this the extent stopped dead at the phi - `let &chosen = if flag then
                 * pick(x, y) else pick(y, x)` followed by a write to `x` was accepted in silence,
                 * because nothing after the merge was reachable from either borrow.
                 */
                pending.push((ModulePtr<Value>)user);
            } else if(auto index = analysis.indexOf.get(U32(user))) {
                auto block = analysis.local[instruction.block];

                if(block->index < extent.blocks.size()) {
                    auto& row = extent.blocks[block->index];

                    if(!row.used || index.unwrap() > row.lastUse) {
                        row.used = true;
                        row.lastUse = index.unwrap();
                    }

                    if(block->index != carrierBlock) {
                        row.liveIn = true;
                        for(auto predecessor: block->incoming(analysis.local)) {
                            markLiveOut(analysis, extent, carrierBlock, visited, predecessor);
                        }
                    }
                }
            }

            /*
             * A borrow written into a closure's environment is live for as long as the closure is.
             *
             * Design-Memory §8: "a by-reference capture is a mutable borrow live for as long as the
             * closure, so while such a closure exists the captured binding may not be borrowed again
             * by the enclosing frame". The extent therefore follows the storage the borrow was
             * written into, then the address of that storage, then the function value the address
             * became a word of - which is exactly the chain a capture takes to reach a `calldyn`.
             */
            Place carrier;
            auto storedInto = instruction.kind == Value::Init && firstPlace(instruction, carrier);
            auto derivedFrom = (instruction.kind == Value::Address || instruction.kind == Value::Borrow) &&
                               firstPlace(instruction, carrier);

            /*
             * A raw pointer read out of borrowed storage is live for as long as whatever holds it.
             *
             * Implementation-Containers.md §4: borrowing `[T]` copies the run's base address and the
             * length into a descriptor, and hands *that* over. The borrow's own last use is the read
             * - so without this the loan would end before the call that is about to index through
             * the address it produced, and the drop pass would be entitled to release the run first.
             *
             * Narrow on purpose. Only a *pointer* read through the borrow extends it, because only a
             * pointer still names the storage after the read; an `Int` copied out of a borrowed
             * record is a value with no relationship to where it came from, and following that would
             * keep every field read's owner borrowed to the end of the function.
             */
            auto aliases = instruction.kind == Value::LoadPlace &&
                           isPointer(analysis.global, instruction.type);

            if(aliases) pending.push((ModulePtr<Value>)user);

            if(storedInto || derivedFrom) {
                auto root = rootLocal(analysis, carrier);

                if(root != maxLimit<U32>) {
                    auto slot = analysis.function.localAt(analysis.local, root);

                    // Into an environment, or into the function value the environment ends up in -
                    // and into anything at all when what is written is one of the aliasing pointers
                    // above, which is how the slice descriptor carries the loan to its call site.
                    auto carries = slot.closureEnv || isFunction(analysis.global, slot.type) ||
                                   isPointer(analysis.global, analysis.local[value]->type);

                    if(storedInto && carries && slot.value) pending.push(slot.value);

                    // Out of an environment: the address is what the function value holds.
                    if(derivedFrom && slot.closureEnv) pending.push((ModulePtr<Value>)user);
                }
            }

            /*
             * A loan handed to a call's `return` group outlives the call, whichever way the callee
             * was named.
             *
             * A direct call reads the group off the callee's summary and a call through a function
             * value reads it off the signature, which is the same contract in the two places it can
             * be written down - and the reason FunArg carries the marker at all. A chain of
             * selectors keeps the original root borrowed for the whole chain either way.
             */
            U64 roots = 0;

            if(instruction.kind == Value::Call) {
                auto summary = summaryOf(analysis, ((InstCall&)instruction).callee);
                if(summary) roots = summary->declaredRoots;
            } else if(instruction.kind == Value::CallDyn) {
                auto signature = ((InstCallDyn&)instruction).signature;
                if(signature && analysis.global[signature]->kind == Type::Fun) {
                    roots = ((FunType*)analysis.global[signature])->returnRoots;
                }
            }

            if(!roots) continue;

            U16 position = 0;
            auto args = instruction.kind == Value::Call ? &((InstCall&)instruction).args
                                                        : &((InstCallDyn&)instruction).args;

            for(auto arg: args->contents(analysis.local)) {
                if(arg == value && (roots & rootBit(position))) pending.push((ModulePtr<Value>)user);
                position++;
            }
        }
    }
}

void checkBorrows(Analysis& analysis) {
    // One extent, refilled per borrow. It is a walk of the block graph, so what it holds is
    // proportional to the function rather than to the borrow, and there is nothing in it worth
    // building twice.
    BorrowExtent extent;

    for(Size at = 0; at < analysis.instructionCount; at++) {
        auto pointer = analysis.order[at];
        auto& borrowed = (InstBorrow&)*analysis.local[pointer];
        if(borrowed.kind != Value::Borrow) continue;

        computeExtent(analysis, pointer, extent);

        /*
         * Block by block rather than straight down the numbering, which is the fix - see
         * BorrowExtent. What is scanned inside each block is the part of it the borrow is actually
         * live across:
         *
         *  - from the top where the borrow reaches the block along an edge, and from the borrow
         *    itself in the block that took it;
         *  - to the bottom where it leaves along an edge, and to its last use in the block where
         *    it dies.
         *
         * A block it is live neither in nor out of and uses in nowhere is not scanned at all, which
         * is every block of a loop the borrow was taken after.
         */
        for(Size b = 0; b < analysis.blockCount() && b < extent.blocks.size(); b++) {
            auto& row = extent.blocks[b];
            auto defines = b == extent.defBlock;
            if(!row.liveIn && !row.liveOut && !row.used && !defines) continue;

            auto& range = analysis.blockRanges[b];
            auto first = row.liveIn || !defines ? range.first : extent.defIndex + 1;
            auto end = row.liveOut ? range.end : (row.used ? row.lastUse + 1 : first);

            for(Size i = first; i < end; i++) {
                auto other = analysis.order[i];
                auto& instruction = *analysis.local[other];

                Place places[kMaxPlaces];
                auto touched = instructionPlaces(instruction, places);
                if(!touched) continue;

                auto overlaps = false;
                for(Size p = 0; p < touched; p++) {
                    overlaps = overlaps || placesOverlap(analysis.local, borrowed.place, places[p]);
                }

                if(!overlaps) continue;

                // The instructions that consume the borrow reach the storage *through* it, which is
                // the whole point of handing one out rather than a conflict with it.
                auto consumed = false;
                for(auto user: analysis.local[pointer]->uses(analysis.local)) {
                    if(user == other) consumed = true;
                }

                if(consumed) continue;

                auto otherBorrow = instruction.kind == Value::Borrow;
                auto otherMutable = otherBorrow && ((InstBorrow&)instruction).mut;

                // Two immutable borrows of one place are exactly what borrows are for.
                if(!borrowed.mut && otherBorrow && !otherMutable) continue;

                // Reading through a live immutable borrow is fine; it is the mutable one that is
                // exclusive. A write is a conflict with either.
                auto writes = instruction.kind == Value::Assign || instruction.kind == Value::Init ||
                              instruction.kind == Value::Move || otherMutable ||
                              instruction.kind == Value::Address ||
                              instruction.kind == Value::Swap || instruction.kind == Value::Exchange;

                if(!borrowed.mut && !writes) continue;

                report(analysis,
                       borrowed.mut
                           ? "this use conflicts with a mutable borrow of the same storage, which is exclusive while it is live"_v
                           : "this write conflicts with an immutable borrow of the same storage that is still live"_v,
                       instruction.source);

                note(analysis, "the borrow it conflicts with is here"_v, borrowed.source);
            }
        }
    }
}

/*
 * Whether a move out of this place empties the slot rather than half of it.
 *
 * The empty path is the obvious one. The other is a lone `Downcast`, and it is not an exception to
 * the rule so much as the case where the rule's premise does not hold: a record's storage is a
 * discriminant and one payload, the discriminant owns nothing, and on the path a `Just(->v)` reached
 * there is no second payload to be left behind. So the slot really is empty afterwards, and every
 * later use of it is rejected and every drop of it skipped, which is what "moved" has to mean.
 *
 * Two steps do not qualify, and both matter. `Downcast` then `Field` is one member of a payload
 * written with braces, which is a genuine partial move. `Downcast` then `Deref` is a boxed payload -
 * `project` appends that `Deref` - and there the box is storage of its own: taking the target out
 * leaves the allocation with nothing to free it, since the reclaim that would have is the one this
 * move just told the compiler not to run.
 */
static bool wholeMove(Analysis& analysis, Place place) {
    auto count = place.projections.size();
    if(count == 0) return true;
    if(count > 1) return false;

    return place.projections.get(analysis.local, 0).kind == ProjectionKind::Downcast;
}

/*
 * What storage a departing operand names, and whether this frame owns it.
 *
 * The one question checkTransfer asks. It used to be asked twice, because a handover reaches a
 * departure point in two shapes and each half derived the answer its own way - the place half from
 * the root, the other from `backingLocal` - which is §0's shape exactly, and it had already gone
 * wrong once in the way recorded above `checkTransfer`.
 *
 * `place` is the only thing the two shapes may still differ by afterwards, and what it decides is
 * which diagnostic is right rather than whether there is one. See the reports.
 */
struct TransferSource {
    // The tracked slot, where there is one. `maxLimit<U32>` covers both a root that is not a local
    // at all and a value with no slot behind it, which is why `owned` is recorded beside it rather
    // than derived from it: a borrow root has no local and is not owned, and a fresh call result
    // has no local and is.
    U32 local = maxLimit<U32>;

    // Whether this frame is the one responsible for releasing what the operand names.
    bool owned = false;

    // A raw pointer's target - `%T` carries no owner, so nothing here has an opinion about it.
    bool outsideModel = false;

    // Set where the operand is a read of a place, and null where it is the value that produced an
    // aggregate. Not "which shape was it" for its own sake: a place can be told to write `->` on
    // the binding that names it, and a borrowed parameter travelling as its `Arg` has no binding to
    // write it on.
    Place* place = nullptr;
};

static TransferSource transferSource(Analysis& analysis, ModulePtr<Value> value) {
    TransferSource result;

    /*
     * The handover reached without a load at all.
     *
     * An aggregate parameter travels as its `Arg` rather than as a read of a place - the rule
     * analyze_effects states as "aggregates travel through the IR as the value that produced them"
     * - so `fn f(v: Held, &out: Held): out = v` is `assign %out, %v` with no LoadPlace anywhere.
     * transferFrom does find the slot and marks it moved, which is right for a slot this frame owns
     * and worse than useless for one it does not: the caller still owns what it lent, and the
     * destination now owns it too.
     *
     * A value with no slot behind it owns itself - a call result the resolver did not give storage,
     * a construction - so there is nothing here for this frame not to own.
     */
    if(analysis.local[value]->kind != Value::LoadPlace) {
        result.local = backingLocal(analysis, value);
        result.owned = result.local == maxLimit<U32> || analysis.tracked[result.local].owned;
        return result;
    }

    auto& place = ((InstLoadPlace*)analysis.local[value])->place;
    result.place = &place;

    /*
     * A raw pointer's target has no owner, so nothing here has an opinion about *whose* it is - but
     * it is still a value, and duplicating one with a teardown is still duplicating it.
     *
     * `owned` is set for exactly that reason. The three "this frame only borrows it" messages below
     * are about a lender, and a pointer has none to name; the copy check under them is about the
     * value, and it applies. Leaving `owned` false sent a pointer target down a branch whose
     * sentence would have been false and out the other side unchecked.
     */
    if(place.root == PlaceRoot::Pointer) {
        result.outsideModel = true;
        return result;
    }

    result.local = rootLocal(analysis, place);
    result.owned = place.root != PlaceRoot::Borrow && place.root != PlaceRoot::Global &&
                   (result.local == maxLimit<U32> || analysis.tracked[result.local].owned);
    return result;
}

/*
 * Ownership leaving the frame through a value that was never moved out of anything.
 *
 * The four points where ownership departs - a write into another place, an exchange, a return, and a
 * phi input - all take a *value*, and transferFrom stops at whichever slot that value is the whole
 * contents of. A projected load has no such slot: `p.first` and the `v` a `Held(v)` pattern binds are
 * both a LoadPlace over a path into a slot somebody else still owns, so the handover finds nothing to
 * mark moved, the source is dropped at its last use, and whoever received the bytes drops them too.
 *
 * That is the same statement `InstMove` makes with a projected place, and it is refused for the same
 * reason - a slot half given away needs a drop flag per field, which the state lattice does not have.
 * The only difference is that `->` says it out loud and this does not, so this is where it has to be
 * noticed. Written as a check rather than as a move: turning these into InstMove would make the
 * *accepted* programs depend on a rule that exists to reject, and a partial move is not something to
 * start representing here.
 *
 * Only droppable types, on the same terms as transferFrom itself. Copying the bytes of something
 * nobody is responsible for is a copy, and that is all `let x = p.count` has ever been.
 *
 * The *unprojected* load was skipped here on the reading that transferFrom would have caught it -
 * "whichever slot that value is the whole contents of". It does not: backingLocal matches a slot
 * whose defining value *is* the operand, which is true of a call result or a construction and false
 * of a load. So `b = a` for two owned `Held` locals read `a`, dropped `b`'s old contents, aliased
 * `a`'s into `b`, and then dropped both names - three drops for two objects.
 *
 * One rule for all four departure points, which `eachTransferOperand` is the list of. It briefly
 * had a flag saying which of them it applied to; every caller passed the same answer, so what the
 * flag described was a distinction that had already stopped existing.
 *
 * And one derivation of the rule's one input, which `transferSource` is. The two shapes a handover
 * arrives in used to answer "does this frame own what is departing" separately, in opposite orders,
 * and the three-drops bug above is what a disagreement between them looks like: it is not that one
 * half was wrong, it is that there were two halves to keep right. What survives the merge is the
 * only difference that was ever load-bearing - whether there is a *place* the reader can be pointed
 * at - and it decides which advice is printed rather than whether anything is.
 */
static void checkTransfer(Analysis& analysis, ModulePtr<Value> value, LocationId source) {
    if(!value) return;

    auto from = transferSource(analysis, value);

    /*
     * A raw pointer's target, which has no owner - so the three "this frame only borrows it"
     * messages below have no lender to name and are skipped. The **copy** check under them is a
     * different question and is not skipped, because duplicating a value with a teardown is about
     * the value rather than about whose it was.
     *
     * All of it used to leave here unchecked, on the reading that `return *p` is what Native needs
     * it to be. What that exempted was not the *move* - `let ->x = *p` is an InstMove and never
     * reaches this function at all, which is what `Reclaim(Array(a))` is written on - but the copy.
     * So `fn read(p: %String) -> String = *p` handed the caller a string to release while the memory
     * kept the only other reference to the same run, and `let &y = *p` made a second owner of it in
     * one line, both silently. The identical line over a local has been refused since before
     * pointers had a section of their own.
     *
     * The escape hatch is not closed, it is spelled: the diagnostic names `let ->`, that spelling
     * still means what it always meant, and Native has to say which of the two it is doing. Nothing
     * in the library had to change.
     *
     * The whole of what the pointer names, and only that. A projection through one leaves the root
     * behind - the same line placeOverwriteDrops draws - so what is on the other side is not a value
     * this can speak for, and the fall-through it would reach is "move the whole value instead",
     * which for the environment of a `@lazy` thunk is a sentence about a projection nobody wrote.
     */
    if(from.outsideModel) {
        if(!from.place) return;

        auto projections = from.place->projections;
        if(projections.isNotEmpty()) return;

        auto held = ownershipIn(analysis.module, functionGen(analysis.global, analysis.function),
                                analysis.local[value]->type);
        if(!held.needsTeardown()) return;

        report(analysis, "this copies the bytes of a value with a teardown into another place, so both names would run it - bind it with `let ->` to move it instead, or use `swap` or `exchange` to write into a place that already holds one"_v,
               source);
        return;
    }

    // Asked of the *context* rather than of the type, exactly as sinkValue asks it: an unconstrained
    // `a` owns something inside the body whatever a caller substitutes, and a declared
    // `TrivialCopy(a)` is what makes reading one out a copy. Without that agreement a generic
    // accessor would be rejected here and then compile the copy anyway.
    auto ownership = ownershipIn(analysis.module, functionGen(analysis.global, analysis.function),
                                 analysis.local[value]->type);
    if(!ownership.needsTeardown()) return;

    /*
     * Storage this frame only borrows, handed on to somebody who will release it.
     *
     * Two messages for one rule, and the difference is what the reader can do about it. An operand
     * that is a read of a place has a binding to put `->` on or a borrow to answer instead; one
     * that is a borrowed parameter travelling as its own `Arg` has neither, and the only honest
     * advice is to change the signature.
     *
     * Nothing here ever looks at an *argument*: handing a borrowed value on as one re-borrows it -
     * another immutable borrow, or the one mutable borrow forwarded - and an argument is not a
     * departure point, which is what eachTransferOperand is the list of.
     */
    if(!from.owned) {
        if(from.place) {
            report(analysis, "this hands ownership on out of storage this frame only borrows - take the whole value with `->` in the signature, or answer a borrow instead"_v,
                   source);
        } else {
            report(analysis, "this stores a value this frame only borrows, so the caller and this destination would both run its teardown - take it with `->` in the signature to own it here"_v,
                   source);
        }

        return;
    }

    // A slot this frame owns, departing as the value that produced it. That *is* the move, and
    // transferFrom marks it moved; there is nothing to report and no place to report it about.
    if(!from.place) return;

    auto& place = *from.place;

    /*
     * The whole slot, written into another place. `b = a` and nothing else.
     *
     * Separated from the payload case below because the answer is different, and the difference is
     * that there is no `->` to write here. `->` goes on a *binding* - `let ->b = a` - and a plain
     * assignment has no binding to put it on: overwriting `b` has to release what `b` held and take
     * what `a` held in one step, which is a move-assign the language does not spell. So the two
     * things that do exist are what this names, and it does not invent a third.
     *
     * `Copy` is deliberately not suggested by name: it is authored per type and most droppable
     * types do not have one, so naming it would send readers looking for something usually absent.
     */
    if(place.projections.isEmpty()) {
        report(analysis, "this copies the bytes of a value with a teardown into another place, so both names would run it - bind it with `let ->` to move it instead, or use `swap` or `exchange` to write into a place that already holds one"_v,
               source);
        return;
    }

    /*
     * A payload the pivot is the only owner of: the move is representable, it just was not written.
     * Pointing at the spelling is the whole value of separating this case out.
     *
     * Both spellings, which is the other half of Analysis-Language.md §2c. `->` alone gives an
     * immutable binding, so a reader who needs to write through the name afterwards would be sent
     * straight back here by the next diagnostic; `&->` is the same move into storage this binding
     * may write. The projection out of an unnamed temporary no longer reaches this at all - see
     * movableTemporary - so what is left here is a place that keeps a name of its own, which is
     * exactly the case where the reader has to choose.
     */
    if(wholeMove(analysis, place)) {
        report(analysis, "this hands ownership on out of a value that still owns it - write `->` on the name that binds it, as in `Just(->v)`, to take it out, or `let &->` where the name must also be writable"_v,
               source);
        return;
    }

    report(analysis, "cannot move a part of a value out of it - move the whole value instead"_v, source);
}

/*
 * Every place one instruction reads, for the use-after-move check over borrowed storage.
 *
 * The first ownership lattice needs nothing like this: `computeEffects` already records a use of
 * the *local* a place is rooted in, and reading a local that was moved out of is exactly what the
 * loop over `effects.uses` reports. A borrow-rooted place has no such local - what its read
 * records is the local the access path starts in, and that local is as initialized as it ever was,
 * so a read of storage this body has emptied looked like a read of a perfectly good environment
 * pointer. This is what asks the second lattice the same question the first one is asked.
 *
 * An unprojected `Init` or `Assign` is left out because it is the *fill*, and filling a slot that
 * was moved out of is the point. A projected one is a read: it writes one field and leaves the
 * rest, so the rest had better still be there.
 */
template<class F>
static void eachReadPlace(Inst& instruction, F&& visit) {
    switch(instruction.kind) {
        case Value::LoadPlace: visit(((InstLoadPlace&)instruction).place); break;
        case Value::Aggregate: visit(((InstAggregate&)instruction).place); break;
        case Value::Borrow:    visit(((InstBorrow&)instruction).place); break;
        case Value::Move:      visit(((InstMove&)instruction).place); break;
        case Value::Exchange:  visit(((InstExchange&)instruction).place); break;
        case Value::Copy:      visit(((InstCopy&)instruction).place); break;
        case Value::Drop:      visit(((InstDrop&)instruction).place); break;
        case Value::Address:   visit(((InstAddress&)instruction).place); break;

        case Value::Swap:
            visit(((InstSwap&)instruction).a);
            visit(((InstSwap&)instruction).b);
            break;

        case Value::Init:
        case Value::Assign: {
            auto& write = (InstInit&)instruction;
            auto projections = write.place.projections;
            if(projections.isNotEmpty()) visit(write.place);
            break;
        }

        default: break;
    }
}

/*
 * Use after move, and the moves that cannot be represented at all.
 */
void checkMoves(Analysis& analysis) {
    auto transfer = [&](ModulePtr<Value> value, LocationId source) {
        checkTransfer(analysis, value, source);
    };

    /*
     * Which borrowed storage this body took ownership out of, and where it left.
     *
     * One list over `BorrowedPlace`, so a `&` binding and a captured `&` are entries of the same
     * kind however differently the state behind each is kept. It stays empty for every body that
     * does not empty a borrow, which is all but a handful - see the release-point loop at the end,
     * which is skipped entirely when it is.
     *
     * The first move rather than the last: it is the one whose obligation the later ones inherit,
     * and it is the line the reader has to change.
     */
    struct Emptied {
        BorrowedPlace place;
        LocationId source;
        bool reported = false;
    };

    SmallArray<Emptied, 2> emptied;

    // Which slots have already had a use-after-move reported - see the loop below.
    auto& reportedUse = analysis.scratch.reportedUse;
    reportedUse.reset(analysis.localCount);

    auto recordEmptied = [&](const BorrowedPlace& place, LocationId source) {
        for(auto& seen: emptied) {
            if(seen.place.local == place.local && seen.place.slot == place.slot) return;
        }

        emptied.push(Emptied { place, source });
    };

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        auto& states = analysis.stateBefore[i];
        auto& effects = analysis.effects[i];

        /*
         * A `ret` counts, because returning a value of type `a` is a hand-over like any other.
         *
         * `fn identity(v: a) -> a = v` promises the caller an `a` it may drop, out of storage the
         * caller lent and still owns. The two honest signatures are `->v`, which takes it, and a
         * `return` marker with a borrow result - `fn identity(return v: a) -> &a` - which promises
         * only a view. What could not stay is the third reading, where the same parameter is
         * borrowed on the way in and owned on the way out.
         *
         * Returning a whole *owned* slot never reaches here: returnValue makes it an InstMove, and
         * a move is the thing this check exists to ask for.
         *
         * A phi's inputs are here too, and used to be a loop of their own over every block's phi
         * list. They departed on the edge into the join rather than at the join, which is where
         * attributePhiEdges puts the *transfer* - but that is a statement about where ownership
         * changes hands, not about where the operand is written, and `numberFunction` puts a phi in
         * the instruction order like everything else. Two loops meant the enumeration had to name
         * which points belonged to which, and naming it once is the whole of eachTransferOperand.
         */
        eachTransferOperand(analysis.local, instruction, transfer);

        if(instruction.kind == Value::Move) {
            auto& moved = (InstMove&)instruction;

            // A partial move would leave the slot half-owned, and every later drop of it would
            // have to know which half. That is a drop flag per field and a drop that runs over a
            // subset of members - real work, deferred deliberately rather than approximated.
            if(!wholeMove(analysis, moved.place)) {
                report(analysis, "cannot move a part of a value out of it - move the whole value instead"_v,
                       instruction.source);
                continue;
            }

            /*
             * Taking ownership out of storage this frame does not own, and putting it back.
             *
             * One rule asked of one thing. `borrowedPlaceOf` is where the two shapes borrowed
             * storage has in the IR are reconciled - a `&` binding, which is a slot in this frame
             * naming the caller's, and a captured `&`, which reaches its storage through a borrow
             * value and has no slot at all. The difference is which lattice carries the state and
             * nothing else, so everything below is written once and holds for both.
             *
             * The move is *allowed* here and settled at the release points at the end of this
             * function, where the state says whether anything was written back. Recorded rather
             * than reported, because the line worth pointing at is this one and the fact that
             * decides it is somewhere else.
             */
            auto borrowed = borrowedPlaceOf(analysis, moved.place);

            if(borrowed.borrowed) {
                if(!borrowed.emptiable) {
                    report(analysis, "cannot take ownership of borrowed storage - a `&` binding never owns what it refers to"_v,
                           instruction.source);
                    continue;
                }

                recordEmptied(borrowed, instruction.source);

                /*
                 * A second move with nothing put back between them, reported only where the
                 * general machinery cannot reach it.
                 *
                 * `effects.uses` is keyed by local, so any place with a row there is already
                 * covered by the use-after-move loop below and saying it here as well states one
                 * mistake twice. Storage reached through a borrow has no such row - what a move
                 * through one *uses* is the local its access path starts in, and that local is as
                 * initialized as it ever was - so this is the only thing that will say it.
                 */
                if(borrowed.local == maxLimit<U32> &&
                   borrowedStateAt(analysis, i, borrowed) != OwnState::Owned) {
                    report(analysis, "this storage has already been moved out of and nothing has been written back to it"_v,
                           instruction.source);
                }

                /*
                 * The use loop below is skipped for this instruction, and only for this one.
                 *
                 * What a move out of borrowed storage *uses* is the storage it is taking, and the
                 * load that produced the value already said so one instruction earlier - at the
                 * token the reader wrote, where this instruction's source is the binding it goes
                 * into. Letting both speak reports one mistake twice, in the wrong order, and the
                 * second of the two points at the name receiving the value rather than at the one
                 * that has nothing left to give.
                 */
                continue;
            }

            if(moved.place.root == PlaceRoot::Global) {
                report(analysis, "cannot take ownership of a global - its storage outlives every frame that could take it"_v,
                       instruction.source);
                continue;
            }
        }

        /*
         * The same question the loop below asks of a local, asked of storage that has no local to
         * be asked about - see eachReadPlace, and the double-move check above for the same
         * division of labour. Skipped outright for bodies with no borrow slots, which is nearly all
         * of them.
         */
        if(analysis.borrowSlots.isNotEmpty()) {
            eachReadPlace(instruction, [&](const Place& place) {
                auto read = borrowedPlaceOf(analysis, place);
                if(!read.emptiable || read.local != maxLimit<U32>) return;

                auto state = borrowedStateAt(analysis, i, read);
                if(state == OwnState::Owned) return;

                report(analysis,
                       state == OwnState::Moved
                           ? "this storage has been moved out of and nothing has been written back to it"_v
                           : "this storage may have been moved out of on some paths reaching here"_v,
                       instruction.source);
            });
        }

        for(auto use: effects.uses) {
            if(states[use] == OwnState::Owned || states[use] == OwnState::Uninitialized) continue;

            /*
             * Once per slot, however many uses reach it.
             *
             * `uses` is not a list of places the reader wrote - it is what `useValue` walked to,
             * which climbs from a call's argument back through the load that produced it to the
             * slot the load named. For an owned local the two coincide and always did. For one
             * behind a `&` they do not: reading through a borrow is a load and then a use of what
             * it loaded, so `length(acc)` named `acc` three times and reported it three times, at
             * two columns of the same line.
             *
             * The first is the one to keep. A value that is gone is gone, and every later mention
             * of it is the same mistake with a different column - what the reader has to change is
             * where it left.
             */
            if(reportedUse[use]) continue;
            reportedUse.set(use, true);

            auto name = analysis.tracked[use].name;
            auto moved = states[use] == OwnState::Moved;

            if(name) {
                report(analysis,
                       moved ? "%@ has been moved out of and cannot be used again"_v
                             : "%@ may have been moved out of on some paths reaching here"_v,
                       instruction.source, analysis.context.findName(name));
            } else {
                report(analysis,
                       moved ? "this value has been moved out of and cannot be used again"_v
                             : "this value may have been moved out of on some paths reaching here"_v,
                       instruction.source);
            }
        }
    }

    /*
     * The other half of the rule: what was taken out has to be back before the borrow is.
     *
     * `acc = acc ++ part` through a `&` is safe for a reason the compiler can state rather than
     * guess at - the storage is emptied, a value is written back, and only then does the frame let
     * go of the reference. So this is not a relaxation of the ownership model but the model asked
     * one program point later. What made the blanket refusal look necessary was that the point had
     * no state to be asked at: a borrow root has no row in the first lattice, which is what
     * computeBorrowOwnership exists to give it.
     *
     * Every `ret` is a release point and nothing else is. There is no unwinding in this language -
     * a failed check aborts the process rather than running teardowns on the way out - so the
     * window between the move and the write cannot be observed by anything but this body, and this
     * body reading through it is the use-after-move above. A call taking the same borrow is that
     * same use.
     *
     * `Maybe` is refused rather than flagged. A local resolves one with a drop flag, because the
     * flag lives in the frame that owns the storage; here that frame is the caller's, and this one
     * has nowhere to write a bit the caller would have to read. So a body that fills the slot on
     * one path and returns without filling it on another is a body that has to say which.
     */
    if(emptied.isEmpty()) return;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        if(analysis.local[analysis.order[i]]->kind != Value::Ret) continue;

        for(auto& entry: emptied) {
            // One diagnostic per storage however many returns fail to fill it, since they are all
            // the same mistake stated at the same line.
            if(entry.reported) continue;
            if(borrowedStateAt(analysis, i, entry.place) == OwnState::Owned) continue;

            entry.reported = true;

            /*
             * Worded for the reader rather than for the pass. The release point this was proved at
             * is a `ret`, but the body in front of the reader may be a `for` whose lines were
             * lifted into a continuation without their saying so - so naming the return would send
             * them looking at the wrong brace. What is true in both shapes is that some path leaves
             * here without putting anything back.
             *
             * A captured `&` goes unnamed. The local its path is rooted in is the environment the
             * continuation was handed, and "env" names nothing the reader wrote - the name that
             * would help is the field's, which is a step along the path rather than the root.
             */
            auto name = entry.place.local != maxLimit<U32>
                ? analysis.tracked[entry.place.local].name : StringId {};

            if(name) {
                report(analysis, "%@ is moved out of here and not written back on every path out - the storage belongs to somebody else, who would release a value that has already left"_v,
                       entry.source, analysis.context.findName(name));
            } else {
                report(analysis, "this takes ownership out of borrowed storage and does not write a value back on every path out - the storage belongs to somebody else, who would release what has already left"_v,
                       entry.source);
            }
        }
    }
}

/*
 * The return-root check (Design.md's "Borrows in return position").
 *
 * The declaration is the contract and the body is what has to fit it, so this compares two things
 * the summary already holds: the group the signature declared, and the roots resolving every return
 * path actually found. Nothing here looks at a callee's body - a call's result arrived with the
 * callee's declared group already mapped through the operands, which is what makes provenance
 * compose transitively through a helper without inspecting one.
 *
 * A raw pointer result is held to the same contract, which is Analysis-Language.md §1. The instinct
 * that a `%T` is outside the ownership model is right about the pointer's *target* - that is
 * checkTransfer's `outsideModel`, and it stays - and wrong about where the address came from, which
 * this model computes for a pointer exactly as it computes it for a borrow: refersToStorage tests
 * isPointer, so `slots(r)` already arrives here carrying `r`. Leaving it unchecked was the one place
 * the compiler knew the answer and did not say it, and what it cost was `bytesOf(terminated(p))`
 * reading a path out of storage the allocator had already taken back.
 *
 * Asked of a result that *is* a pointer and not of one that merely contains one, which is the same
 * line containsBorrowLike draws and for the same reason: a raw pointer is where this analysis stops
 * by construction, so `Array(T)` holds a `Run(T)` holds a `%T` and descending would make every
 * container-returning function in the library declare a group. A pointer stored into something
 * longer-lived than its root is the separate hole §1 names at its end, and it is not this one.
 */
void checkReturnRoots(Analysis& analysis) {
    auto& function = analysis.function;
    auto& summary = function.summary;
    if(!summary.returnsBorrow && !isPointer(analysis.global, function.returnType)) return;

    auto source = function.source;

    /*
     * A borrow rooted in a local, a global, or a sunk parameter has no caller-side root that could
     * keep it alive, which is a different mistake from being rooted in the wrong argument.
     *
     * Asked only of a result that *is* a reference, while the group check below is asked of one that
     * merely contains one. The asymmetry is provenance being field-insensitive: a record holding both
     * an owned array and a slice has local roots for the array, and they are not a mistake - the
     * array goes with the result. Reporting on them would reject `data Mixed {view: &[Int], owned:
     * [Int]}` for the half that is doing nothing wrong.
     *
     * Something *is* lost by that, and it is worth naming rather than leaving as a claim that the
     * slice case covers it. checkEscapingViews asks about a descriptor's own slot, so it catches a
     * record holding a view of this frame's container; it says nothing about a record holding a
     * plain `&T`. So this is accepted and should not be:
     *
     *     fn escaping() -> Pair(&Int, &Int):
     *         let &a = 3 :: Int
     *         let &b = 4 :: Int
     *         return make(a, b)
     *
     * `Pair(&Int, &Int)` is not `isBorrowLike`, so the arm above is skipped; `actualRoots` is empty
     * because `a` and `b` are locals rather than arguments; and nothing else asks. The result names
     * two dead stack slots and the program is compiled.
     *
     * The missing fact is field sensitivity, and no rearrangement of these checks supplies it: the
     * question "is this local root reached through a borrow, or is it the owned half" is one
     * provenance deliberately does not keep an answer to. Analysis-Borrows.md §2.4 and §6.6 are
     * where the replacement is - a loan slot on the semantic type, which knows the path each
     * reference sits at - and this comment is the marker for what that work has to close.
     */
    if(summary.invalidRoot && isBorrowLike(analysis.module, function.returnType)) {
        report(analysis,
               "a borrow returned from this function is rooted in storage the caller does not own - it must come from an argument marked `return`"_v,
               source);
    }

    auto undeclared = summary.actualRoots & ~summary.declaredRoots;
    if(!undeclared) return;

    // One rule, and the noun is the only thing that differs. A reader told a *pointer* is unrooted
    // has been told what to look at; told a "borrow" about a `-> %U8` they would look for a `&`
    // that is not there.
    auto pointerResult = isPointer(analysis.global, function.returnType);

    U16 index = 0;
    for(auto argPointer: function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];

        if(undeclared & rootBit(index)) {
            report(analysis,
                   pointerResult
                       ? "a pointer returned from this function is rooted in %@, which the signature did not mark `return`"_v
                       : "a borrow returned from this function is rooted in %@, which the signature did not mark `return`"_v,
                   arg->source, analysis.context.findName(arg->name));
        }

        index++;
    }
}

/*
 * A loan that left the extent its signature gave it.
 *
 * Five things used to ask this, one function each, and every one of them opened by reading
 * `analysis.escaped` and then differed only in which slot it was looking at and what it said. They
 * are one statement - *something borrowed outlived what was keeping it valid* - and the useful part
 * is not the loop but the answer to the second question: **what said this loan's extent**. There
 * are two kinds of answer, and that is the division below rather than five.
 *
 * A *declaration* says it, on behalf of implementations no call site can see. A class function's
 * borrowed parameter and a class iterator's continuation are both of these: a generic body cannot
 * see which instance runs, so it takes the declaration's word, and what makes the declaration true
 * is checking every implementation against it here. See assumedRetained in analyze_escape.cpp,
 * which is the assumption these pay for.
 *
 * Or this frame's *own storage* says it, and then the reason is a property of the slot: a slice is
 * a view of a container this frame is about to tear down, a widened `&` argument is a temporary the
 * return would have narrowed back, a closure environment holds a reference to the frame that built
 * it. Three slot flags, three sentences, one loop.
 *
 * All of this is what Analysis-Borrows.md §6.3 calls "an ad-hoc repair at one dispatch mechanism
 * instead of the meaning of a borrowed parameter", and §8.4 replaces the lot with ordinary
 * signature conformance once a loan is part of a type. Collapsing them first is what makes that one
 * edit rather than five: the reasons stay, the five entry points do not.
 */

// Which promise one parameter is held to, or None where this function made none - which is every
// function that does not implement a class.
enum class DeclaredExtent: U8 {
    None,

    // An ordinary borrowed parameter of a class implementation or a class default.
    Parameter,

    /*
     * The continuation a `for` loop appends. It is the last parameter, always - the desugaring puts
     * it there, so an implementation cannot have written one of its own after it - and it is a
     * separate answer because it is the one a caller could not have declared and the one `->`
     * cannot rescue. See Function::classContinuation and emitGenericDispatch.
     */
    Continuation,
};

static DeclaredExtent declaredParameterExtent(Analysis& analysis, U16 index) {
    auto& function = analysis.function;

    if(function.classContinuation && index + 1 == function.args.size()) {
        return DeclaredExtent::Continuation;
    }

    // A class *default* is held to the rule as well, and has to be: an instance that supplies no
    // implementation puts the default in the slot, so a deferred dispatch reaches it exactly as it
    // reaches a written one. See Function::classDefault.
    if(function.instanceOf || function.classDefault) return DeclaredExtent::Parameter;

    return DeclaredExtent::None;
}

// The parameters a declaration promised would not be retained, and were.
static void checkDeclaredExtents(Analysis& analysis) {
    auto& function = analysis.function;
    if(!function.instanceOf && !function.classDefault && !function.classContinuation) return;

    U16 index = 0;

    for(auto argPointer: function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];
        auto kind = declaredParameterExtent(analysis, index);
        index++;

        // `->` is both the escape hatch and the answer to the diagnostic: a member that has to
        // store what it is given asks for it by value. A continuation has no such hatch, which is
        // why it is a separate sentence rather than a separate check.
        if(kind == DeclaredExtent::None || arg->convention == ast::BindType::Sink) continue;

        auto slot = backingLocal(analysis, (ModulePtr<Value>)argPointer);
        if(slot == maxLimit<U32> || !analysis.escaped[slot]) continue;

        if(kind == DeclaredExtent::Continuation) {
            report(analysis, "this implements a class %@, so its continuation cannot outlive the call - a `for` loop in a generic body cannot see which implementation runs, and takes the declaration's word that the continuation is called rather than stored"_v,
                   arg->source,
                   function.funKind == ast::FunKind::Iter ? "`iter fn`"_v : "`lens fn`"_v);
        } else {
            report(analysis, "this implements a class function, so it cannot keep %@ beyond the call - a call in a generic body cannot see which implementation runs, and takes the declaration's word that a borrowed argument's extent is the call. Declare the parameter `->` if the body has to store what it is given"_v,
                   arg->source, analysis.context.findName(arg->name));
        }
    }
}

/*
 * One escaped local, and what this frame was keeping it valid with.
 *
 * The three flags are mutually exclusive by construction - a closure environment is not a view and a
 * view is not a materialized temporary - so the order they are tested in is a formality rather than
 * a precedence, and the early returns say that rather than implying a fallthrough that cannot
 * happen.
 */
static void checkFrameExtent(Analysis& analysis, Size local) {
    auto global = analysis.global;
    auto slot = analysis.function.localAt(analysis.local, U32(local));
    auto source = slot.value ? analysis.local[slot.value]->source : analysis.function.source;

    /*
     * The one borrow that is still a temporary, escaping.
     *
     * A `&` argument whose declared type is wider than the field's - `increment(&x: Int)` given a
     * `@bits(13)` field - is widened into a temporary and narrowed back when the call returns,
     * because a reference cannot convert. That is right for a callee that reads and writes through
     * it and wrong for one that keeps it: the temporary dies with the frame, and nothing would ever
     * narrow.
     *
     * Nothing about packing is left in this check. A borrow of a narrow field at its *own* type is a
     * reference that carries the field's shift, works wherever the field is, and outlives whatever
     * it likes - see NarrowRef in resolve/lower.cpp. What is reported here is a conversion with
     * nowhere to happen, and the fix is in the signature.
     */
    if(slot.materialized) {
        report(analysis, "cannot borrow this beyond the call - the callee declared a wider type than the field has, so what it receives is a temporary written back when the call returns, and this callee keeps it. Declare the parameter at the field's own type"_v,
               source);
        return;
    }

    /*
     * A slice may not outlive the container it is a view of - Implementation-Containers.md §4.
     *
     * The descriptor holds a `%T` into the run, and a raw pointer is outside the ownership model by
     * construction, so nothing about the *fields* of a slice says it refers to anything. What says
     * so is Local::viewOf, written where the descriptor was built out of an owned array, and this is
     * the check that spends it: the slice escaped this frame while the array it points into is this
     * frame's, so the frame is about to run that array's teardown and free the run underneath it.
     *
     * Precise rather than provenance-shaped, deliberately. Asking "does the returned value contain a
     * borrow" would have to go through the containment relation, which is field-insensitive - so a
     * record holding both an owned array and a slice would be rejected for the owned half. This asks
     * about the slice's own slot and nothing else, and a slice's slot is only ever a view when this
     * frame made it one.
     *
     * A slice that came *in* as a parameter has no `viewOf` and is not reported here: what it refers
     * to is the caller's, and whether the caller may hand it on is the caller's own return-root
     * check.
     */
    if(slot.viewOf != maxLimit<U32>) {
        auto viewed = analysis.function.localAt(analysis.local, viewedRoot(analysis, slot.viewOf));

        /*
         * A view of a *parameter's* container is the return-root check's business, not this one's.
         *
         * What this is about is a container this frame owns and is about to tear down; a parameter's
         * is the caller's, outlives the call, and is exactly what
         * `fn elements(return self: Array(a)) -> '[a]` hands a view of -
         * Implementation-Containers.md §5. Whether *this* signature is allowed to hand it back is
         * one question with one answer, and checkReturnRoots is where it is asked: deriveSummary
         * walks the same `viewOf` chain, so a view of an argument the signature did not mark
         * `return` is reported there and naming the argument. A sunk parameter is not exempt - the
         * callee owns what it was given, so its teardown is this frame's like any other local's.
         */
        auto owner = viewed.value && analysis.local[viewed.value]->kind == Value::Arg
            ? (Arg*)analysis.local[viewed.value] : nullptr;

        if(owner && owner->convention != ast::BindType::Sink) return;

        auto viewSource = slot.value ? analysis.local[slot.value]->source : analysis.function.source;

        if(viewed.name) {
            report(analysis, "this borrow of %@ outlives the frame that owns it - a slice is a view into the container's storage, and the container is released when this function returns"_v,
                   viewSource, analysis.context.findName(viewed.name));
        } else {
            report(analysis, "this borrow of an array outlives the frame that owns it - a slice is a view into the container's storage, and the container is released when this function returns"_v,
                   viewSource);
        }

        return;
    }

    /*
     * A closure that outlives the frame cannot hold a borrow of it.
     *
     * Design-Memory §8's third case says a closure that must outlive the frame that built it has to
     * own what it captures, and this is where that is checked: the environment escaped, so anything
     * in it that is a `&T` names storage this frame is about to stop guaranteeing. The capture
     * conventions are chosen before any of this is known - a capture is decided at the name that
     * made it, and whether the closure escapes is a whole-function fact - so the two meet here
     * rather than at the lambda.
     *
     * A closure that is merely *called* does not trip this. Nothing marks the environment escaped at
     * an InstCallDyn, deliberately: a lifted body has no way to name its own environment, so it
     * cannot store one, and treating every call as a handover would reject every closure that is
     * used.
     */
    if(slot.closureEnv && slot.type && global[slot.type]->kind == Type::Tup) {
        for(auto field: ((TupType*)global[slot.type])->fields.contents(global)) {
            /*
             * A captured slice is a captured reference - see isBorrowLike - and it is reported
             * separately because the two have nothing to say to each other. A `&T` capture is about
             * the *convention* the capture was made under, which is what the enclosing binding's
             * mutability decided; a slice is a reference whatever convention it travelled by, since
             * copying the descriptor copies the address inside it.
             */
            if(isBorrow(global, field.type)) {
                report(analysis, "this closure outlives the frame that built it, so it cannot capture %@ by reference - the enclosing binding is %@, and a capture of mutable storage is always by reference (Design-Memory §8)"_v,
                       source, analysis.context.findName(field.name),
                       ((BorrowType*)global[field.type])->mut ? "mutable"_v : "borrowed from somewhere else"_v);
            } else if(isBorrowLike(analysis.module, field.type)) {
                report(analysis, "this closure outlives the frame that built it, so it cannot capture %@ - it is a slice, which is a view into a container someone else owns, and copying the descriptor copies the address inside it (Design-Memory §8)"_v,
                       source, analysis.context.findName(field.name));
            }
        }
    }
}

void checkLoanExtents(Analysis& analysis) {
    for(Size l = 0; l < analysis.localCount; l++) {
        if(analysis.escaped[l]) checkFrameExtent(analysis, l);
    }

    checkDeclaredExtents(analysis);
}
