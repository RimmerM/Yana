#include "analyze_internal.h"
#include "own.h"
#include "gen.h"

namespace mir {

namespace {

// What the walk over a block is being asked for. One function and three readings of it, so that the
// fixpoint, the flag discovery and the emission cannot come to disagree about where a drop goes -
// which they would the moment the second of them was a copy of the first with a line changed.
enum class WalkMode: U8 {
    // Inside the state fixpoint: no diagnostics, no steps, and no flag decisions. Runs many times.
    Fixpoint,

    // Once over settled states, to find which paths are released conditionally and therefore need a
    // drop flag. Before emission, because a flag write has to be emitted at every transition of a
    // path the *last* block turns out to need one for.
    Discover,

    // Once more, emitting the steps and reporting the program's own mistakes.
    Emit,
};

} // namespace

/*
 * One block, forwards - the initialization dataflow (§6.3) and the drop placement (§6.7) together.
 *
 * They are one walk because they are one question asked at each position: what is owned here, and is
 * it still wanted? The second half of that is liveness, which is already settled, so the whole of
 * drop placement is the sentence
 *
 *     an object is released at the first point at which it is owned and no longer live
 *
 * and the four cases a reader expects to find written out - the end of a scope, the arm of an `if`
 * that did not use it, the assignment that replaces it, the value carried out of a loop - are that
 * one sentence meeting four control-flow shapes. A pass with a rule per shape is how the current
 * compiler's drop placement grew to four files.
 *
 * `state` is updated in place and is the block's exit state when this returns.
 */
void Analysis::walkBlock(U32 index, PathState* state, int mode) {
    auto walk = WalkMode(mode);
    auto count = nodeCount(index);
    if(!count) return;

    /*
     * Liveness at every position, recomputed from this block's live-out.
     *
     * The fixpoint stores one answer per block; what a drop needs is one per position. Stepping the
     * same backward transfer over the block is what produces it, and re-deriving it here rather than
     * storing it is the §8.6 trade: a table of blocks times positions times paths would be the
     * largest thing this analysis allocates, for an answer that is used once.
     */
    SmallArray<U64, 32> after;
    for(U32 i = 0; i < count * pathWords; i++) after.push(0);

    {
        SmallArray<U64, 8> live;
        auto out = liveAt(liveOut, index);
        for(U32 w = 0; w < pathWords; w++) live.push(out[w]);

        OriginCursor cursor { *this };
        cursor.enterAtExit(index);

        for(U32 n = count; n > 0; n--) {
            auto position = n - 1;
            BitMatrix::copy(after.pointer() + position * pathWords, live.pointer(), pathWords);
            cursor.undo(position);
            applyBackward(live.pointer(), cursor, nodeAt(index, position));
        }
    }

    auto liveBefore = [&](U32 position, U32 p) {
        if(position == 0) return BitMatrix::test(liveAt(liveIn, index), p);
        return BitMatrix::test(after.pointer() + (position - 1) * pathWords, p);
    };

    auto setSubtree = [&](MovePathId id, PathState value) {
        SmallArray<MovePathId, 16> pending;
        pending.push(id);

        while(pending.size()) {
            auto current = pending.pop().unwrap();
            state[current.index] = value;
            for(auto child: path(current).children.contents(base)) pending.push(child);
        }
    };

    /*
     * Which field of its parent a path names, for the sum rule below, and `kNoField` where it names
     * something else.
     *
     * A move path is built from a place, so this is the place's own payload - but only a
     * `Place::Field` stores a field index there; a `Place::Property` stores a requirement's position
     * and is the other kind of child a record's path can have. A property of a sum cannot be written
     * today, because a field of one is not reachable by name, and answering "not a field" is what
     * keeps that a rule rather than an assertion somebody trips over later.
     */
    static constexpr U16 kNoField = 0xffff;

    auto fieldOf = [&](MovePathId id) {
        auto place = body.place(base, path(id).place);
        return place->kind == Place::Field ? U16(place->payload) : kNoField;
    };

    /*
     * A move out of a field leaves the record as a whole gone and its other fields where they were,
     * which is the whole of partial ownership - see `PathState`.
     *
     * A **sum** is the one record where the surviving fields are not the siblings: the arms share
     * their storage, so what is left after a take out of arm *k* is the rest of arm *k* and nothing
     * at all of any other. That is not an approximation - a field of a sum is reached by matching
     * and by nothing else, so a take out of one *proves the discriminant*, and on this path the
     * other arms are not there to be released. Saying so here is what lets the walk descend into a
     * partially moved sum at all; without it a `Kept(->g)` would leave `Empty`'s fields owned and
     * release storage the take had just emptied.
     */
    auto markAncestorsMoved = [&](MovePathId id) {
        for(auto child = id, up = path(id).parent; up.valid(); child = up, up = path(up).parent) {
            auto field = fieldOf(child);

            if(field != kNoField && isSumRecord(decl, path(up).type)) {
                auto& record = *(RecordType*)decl[path(up).type];
                auto proved = record.constructorOf(decl, field);

                for(auto sibling: path(up).children.contents(base)) {
                    auto other = fieldOf(sibling);
                    if(other == kNoField || record.constructorOf(decl, other) == proved) continue;

                    setSubtree(sibling, PathState::Moved);
                }
            }

            state[up.index] = PathState::Moved;
        }
    };

    /*
     * And the way back: a record every one of whose tracked fields is there again is whole again.
     *
     * The exact inverse of the rule above, and it has to exist for the same reason that one does.
     * `p.left = guard()` over a field that was handed over refills that field; whether it also
     * refills *`p`* is not a fact about the field, it is a fact about every sibling, and this is
     * where the difference between "one field is back" and "the record is back" is decided. Without
     * it a record could be taken apart and put together again and still be unusable as a whole,
     * which is the state a fixture found this in.
     *
     * Only `Owned` counts. A sibling that is conditionally owned leaves the parent conditional at
     * best, and the parent is already `Moved` - which releases the fields one at a time and is the
     * conservative answer rather than the wrong one.
     *
     * The walk stops at the first ancestor that is not made whole, because an ancestor of *that*
     * cannot be either.
     */
    auto markAncestorsRefilled = [&](MovePathId id) {
        for(auto up = path(id).parent; up.valid(); up = path(up).parent) {
            if(state[up.index] == PathState::Owned) break;

            auto whole = true;
            for(auto child: path(up).children.contents(base)) {
                if(state[child.index] != PathState::Owned) whole = false;
            }

            if(!whole) break;
            state[up.index] = PathState::Owned;
        }
    };

    auto flagAt = [&](U32 position, MovePathId id, bool value) {
        if(walk != WalkMode::Emit || !out.flagged.get(base, id.index)) return;
        out.flags.push(program.mir, FlagStep { index, position, id, value });
    };

    for(U32 position = 0; position < count; position++) {
        auto& node = nodeAt(index, position);

        /*
         * The drops, before the node that made them due.
         *
         * Top down over the tree, so that a record that is still whole is released once rather than
         * field by field: a path that is owned and dead is dropped entire, and only a path that has
         * been partially moved is descended into to find the members that are still there.
         */
        SmallArray<MovePathId, 16> pending;
        for(U32 p = 0; p < pathCount; p++) {
            if(path(MovePathId(p)).parent.valid()) continue;
            pending.push(MovePathId(p));
        }

        // Reversed once, so that the pop order below is path order: the drops a fixture reads have
        // to be in an order that does not depend on how a stack happens to unwind.
        for(Size i = 0, j = pending.size(); i + 1 < j; i++, j--) {
            auto held = pending[i];
            pending[i] = pending[j - 1];
            pending[j - 1] = held;
        }

        auto descend = [&](MovePathId id) {
            auto& record = path(id);
            for(Size i = record.children.size(); i > 0; i--) {
                pending.push(record.children.get(base, U32(i - 1)));
            }
        };

        while(pending.size()) {
            auto id = pending.pop().unwrap();
            auto here = state[id.index];

            if(here == PathState::Owned || here == PathState::Maybe) {
                if(liveBefore(position, id.index)) continue;

                /*
                 * The point the value stops being live with nothing in the source having said so -
                 * `doc/spec/ownership.md`'s `@explicitDrop`, reported exactly where it dies.
                 *
                 * Before the teardown branches below rather than beside them, because the attribute
                 * is about *this* event and not about what would have run: a linear type with an
                 * authored `Drop`, one with a derived walk and one with nothing to run at all are
                 * one diagnostic, and it is the compiler having chosen the point.
                 *
                 * The advice is `->` and is worth spelling out, since the fix is a statement the
                 * author has probably never written. `Local::disposal` is what keeps this from
                 * firing on the fix itself: the temporary a `->x` writes into dies here too, and it
                 * is the one death in the language the source did ask for.
                 *
                 * The subject of a teardown is the second exemption and is a different statement:
                 * `instance Drop(Guard)` on an `@explicitDrop` type is the *ordinary* shape of the
                 * feature, and inside that body the subject is not dying unasked - it is dying, and
                 * this body is what the attribute made somebody write. Its members are not exempt,
                 * which is the check the next branch descends for.
                 */
                if(walk == WalkMode::Emit && needsExplicitDisposal(decl, path(id).type) &&
                   !writtenDisposal(id) && id != teardownSubject) {
                    if(here == PathState::Maybe) {
                        /*
                         * Consumed on one path and not on another. A drop flag is what would make
                         * this run, and `doc/spec/ownership.md` refuses one deliberately: a value
                         * that must be disposed of where the source says so may not be disposed of
                         * at a point only a run-time bit knows about.
                         *
                         * No fixture reaches it, and the reason is worth writing down rather than
                         * taking as a sign the branch is dead: a drop is placed at the *earliest*
                         * point a path is owned and dead, so a value forgotten on one arm of an `if`
                         * dies inside that arm and gets the message below. What would reach here is
                         * a path still live out of both arms and dead after the join, which needs a
                         * use after the merge - and a use after the merge on the arm that consumed
                         * it is the handover diagnostic instead. The branch stays because the
                         * message below would be actively wrong here: it says the value is not used
                         * afterwards, and on the other path it is.
                         */
                        report(node.source,
                               "%@ is handed over on some paths that reach here and not on others, "
                               "and %@ may not be disposed of conditionally"_v,
                               pathName(id), typeText(context, decl, path(id).type));
                    } else if(underTeardownSubject(id)) {
                        // The authored teardown's own obligation - see `Analysis::teardownSubject`.
                        // Separated from the message below because the fix is different: this one is
                        // disposed of *in this body*, and there is no later point to move it to.
                        report(node.source,
                               "%@ is still owned when this teardown returns, and %@ may not be "
                               "disposed of implicitly - write `->%@`"_v,
                               pathName(id), typeText(context, decl, path(id).type), pathName(id));
                    } else {
                        report(node.source,
                               "%@ is not used after this, and %@ may not be disposed of "
                               "implicitly - hand it over, or write `->%@` where it should end"_v,
                               pathName(id), typeText(context, decl, path(id).type), pathName(id));
                    }

                    setSubtree(id, PathState::Moved);
                    continue;
                }

                /*
                 * Inside the subject of a teardown, where nothing is released and the walk still has
                 * to reach the members - `Analysis::teardownSubject`.
                 *
                 * The descent is the whole difference from the branch below. Everywhere else a dead
                 * owned record is released whole and never taken apart, because its teardown covers
                 * its members; here there is no teardown to run, so the members have to be visited
                 * one at a time for the check above to see them.
                 */
                if(underTeardownSubject(id)) {
                    state[id.index] = PathState::Moved;

                    /*
                     * Except into a **sum**, whose members are alternatives.
                     *
                     * Nothing has proved which arm this is - a take would have, and this is the path
                     * where none happened - so descending would visit every arm's fields with the
                     * state the construction left them in and demand each of them be disposed of.
                     * What is given up is the linear-member diagnostic *inside* an authored teardown
                     * for a sum, which is the one shape this walk cannot reason about without the
                     * tag; the derived teardown that reads it is `emitSumTeardown`'s.
                     */
                    if(!isSumRecord(decl, path(id).type)) descend(id);
                    continue;
                }

                /*
                 * Of the **occurrence**, so that a value whose members are somebody else's is not
                 * released - `Maybe('v)`, whose payload the signature borrowed.
                 *
                 * The derived teardown is generated per *type*, so it would descend into the payload
                 * and drop storage this frame only refers to. `checkQualifiedShapes` is what makes
                 * asking the occurrence the whole answer rather than half of one: a qualified
                 * argument is admitted only where removing it leaves nothing to run, so there is no
                 * residue for a shape-aware teardown to have to emit.
                 */
                if(hasTeardownAt(decl, body.place(base, path(id).place)->occurrence)) {
                    if(walk == WalkMode::Emit) checkDisposalPermission(node.source, id);

                    /*
                     * The same event at a type this body cannot see - `Disposal` in `decl.h`.
                     *
                     * Both checks above answer for a type that is known here, and a generic body's
                     * is not: `a` withholds nothing and permits everything, because a variable has
                     * no attribute and no instance. So the questions are recorded rather than
                     * answered, and `specializeFunction` asks them of the type a caller supplied -
                     * which is `doc/spec/ownership.md`'s "a question with no fallback has to be
                     * asked at the instantiation", and is the whole of waypoint G4.
                     *
                     * The two exemptions are the concrete check's own, carried in the flag rather
                     * than applied here: a written `->x` and a teardown's own subject are points the
                     * source chose, and only `@explicitDrop` cares which.
                     */
                    if(walk == WalkMode::Emit && !isConcreteType(decl, path(id).type)) {
                        deferDisposal(path(id).type, node.source,
                                      !writtenDisposal(id) && id != teardownSubject);
                    }

                    if(here == PathState::Maybe && walk == WalkMode::Discover) {
                        out.flagged.set(base, id.index, 1);
                    }

                    if(walk == WalkMode::Emit) {
                        out.drops.push(program.mir, DropStep {
                            index, position, id, here == PathState::Maybe
                        });
                    }
                }

                flagAt(position, id, false);
                setSubtree(id, PathState::Moved);
                continue;
            }

            // Gone as a whole, and its members may not be: this is where a partially moved record is
            // descended into.
            descend(id);
        }

        /*
         * The node's own effect on the state, from the operand modes and the destination mode - and
         * from nowhere else. §6.3's "the facts come from operand and destination modes. There is no
         * separately derived `moves` list."
         */
        forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
            if(role != OperandRole::ReadSource) return;
            if(operand.source() != SourceKind::Place) return;

            auto index = operand.placeId().index;
            auto id = exact[index].valid() ? exact[index] : cover[index];
            if(!id.valid()) return;

            if(walk == WalkMode::Emit) {
                switch(state[id.index]) {
                    case PathState::Owned:
                        break;
                    case PathState::Uninitialized:
                        report(node.source, "%@ is read before it has a value"_v,
                               pathName(id));
                        break;
                    case PathState::Moved:
                        report(node.source, "%@ has already been handed over, and this reads it again"_v,
                               pathName(id));
                        break;
                    case PathState::Maybe:
                        report(node.source,
                               "%@ has been handed over on some paths that reach here, and this reads it"_v,
                               pathName(id));
                        break;
                }
            }

            if(operand.mode() != UseMode::Take) return;

            /*
             * A field may not be moved out of a value that releases itself.
             *
             * An authored teardown is a body that runs on the *whole* value, and a whole value is
             * exactly what a record with a field missing is not: there is no point after the move at
             * which that body could be called with something it was written to receive. Nothing else
             * in this analysis can catch it, because the walk is otherwise perfectly happy - it
             * releases the surviving fields one at a time and never reaches the record itself, so
             * the authored body is silently *not run*, which is the one outcome a teardown may not
             * have. Found by `ownership/PartialMove.yana` on a record that counted its own drops.
             *
             * Every ancestor, not just the parent: `n.inner.left` is as much a hole in `n` as in
             * `n.inner`, and the outermost one is the one whose teardown a reader is surprised by.
             *
             * A *derived* teardown is not affected and must not be, since releasing the members one
             * at a time is exactly what a derived teardown does - which is the whole of partial
             * ownership and the reason this stage exists.
             */
            if(walk == WalkMode::Emit) {
                for(auto up = path(id).parent; up.valid(); up = path(up).parent) {
                    /*
                     * The subject of *this* teardown is the one value with an authored body that a
                     * field may be moved out of, because the body is what is running: there is no
                     * later point at which it would be called with something it was written to
                     * receive, since this is that call. `->value.free` in a `Drop` for the record
                     * holding it is the shape the rule below would otherwise refuse, and it is how
                     * an authored teardown discharges a linear member at all.
                     */
                    if(up == teardownSubject) break;

                    auto ownership = ownershipOf(decl, path(up).type);
                    if(ownership.drop != TeardownKind::Authored &&
                       ownership.reclaim != TeardownKind::Authored) continue;

                    report(node.source,
                           "%@ cannot be handed over on its own, because %@ has a teardown that "
                           "runs on the whole value"_v,
                           pathName(id), typeText(context, decl, path(up).type));
                    break;
                }
            }

            setSubtree(id, PathState::Moved);
            markAncestorsMoved(id);
            flagAt(position, id, false);
        });

        forEachOperand(base, node, [&](Operand& operand, OperandRole role) {
            if(role != OperandRole::WriteTarget) return;
            if(operand.source() != SourceKind::Place) return;

            auto id = exact[operand.placeId().index];
            if(!id.valid()) return;

            auto& written = (NodeWrite&)node;
            auto live = state[id.index] == PathState::Owned || state[id.index] == PathState::Maybe;

            /*
             * `Reinit` is the normalized third destination - §5.5, and this is the dataflow that
             * produces it. An assignment whose target is not there any more does not release
             * anything, and a dump that still called it an assignment would be describing a drop
             * that the drop list does not contain.
             *
             * The rewrite is the second and last thing this pass changes in a sealed body, on the
             * same terms as `Forward`: an answer the builder deliberately left open, written back
             * where it was left.
             */
            if(walk == WalkMode::Emit && written.mode == Destination::Assign && !live) {
                written.mode = Destination::Reinit;
            }

            if(walk == WalkMode::Emit && written.mode == Destination::Init && live) {
                report(node.source, "internal: an initialization overwrites live contents"_v);
            }

            setSubtree(id, PathState::Owned);
            markAncestorsRefilled(id);
            flagAt(position, id, true);
        });
    }
}

/*
 * The forward fixpoint over the block graph.
 *
 * Entry state first: a parameter arrives owned because the call established it, and a local arrives
 * with nothing in it. That distinction is `Local::parameter`, and it is the reason a parameter is a
 * local rather than an entry-block argument copied into one - the initialization dataflow has to
 * start it as owned, and there is no `write.init` in the body that would say so.
 */
void Analysis::computeInitialization() {
    auto entries = blockCount * pathCount;

    stateIn.clear();
    stateOut.clear();
    reached.reset(blockCount);

    for(U32 i = 0; i < entries; i++) {
        stateIn.push(PathState::Uninitialized);
        stateOut.push(PathState::Uninitialized);
    }

    if(!pathCount) return;

    auto entry = stateAt(stateIn, 0);
    for(U32 p = 0; p < pathCount; p++) {
        // The *root* of the path, which is what says whether the storage was filled by the call. A
        // parameter's fields arrive owned exactly as the parameter does, so the question is asked of
        // the local the projection walks back to rather than of the projection.
        auto place = body.place(base, path(MovePathId(p)).place);
        while(place->parent) place = base[place->parent];

        if(place->kind != Place::Local) continue;

        auto local = body.local(base, LocalId(place->payload));
        if(local->isParameter()) entry[p] = PathState::Owned;
    }

    reached.set(0, true);

    // Which blocks have been walked at least once. A block whose exit state has not moved is still
    // a reason to visit its successors the first time, because they have no state to have met with.
    IndexSet walked;
    walked.reset(blockCount);

    SmallArray<PathState, 64> merged, state;
    for(U32 p = 0; p < pathCount; p++) { merged.push(PathState::Uninitialized); state.push(PathState::Uninitialized); }

    sweepForward([&](U32 index) {
        auto block = this->block(index);

        if(index != 0) {
            /*
             * The meet over the predecessors that have been reached.
             *
             * Unreached ones are skipped rather than met with, which is what keeps an `if` whose
             * arms both `return` from making the join look conditionally owned: a block nothing
             * enters contributes no state, and a block that contributes its bottom would drag every
             * path in the successor to `Maybe`.
             */
            auto first = true;

            for(auto predecessor: block->incoming.contents(base)) {
                auto from = base[predecessor]->index.index;
                if(!reached.get(from)) continue;

                auto out = stateAt(stateOut, from);
                for(U32 p = 0; p < pathCount; p++) {
                    merged[p] = first ? out[p] : meetState(merged[p], out[p]);
                }

                first = false;
            }

            if(first) return;
            reached.set(index, true);

            auto entered = stateAt(stateIn, index);
            for(U32 p = 0; p < pathCount; p++) entered[p] = merged[p];
        }

        auto in = stateAt(stateIn, index);
        for(U32 p = 0; p < pathCount; p++) state[p] = in[p];

        walkBlock(index, state.pointer(), int(WalkMode::Fixpoint));

        auto out = stateAt(stateOut, index);

        // `add` answers whether the block was not already there, so the first visit is a change: the
        // successors have no state to have met with yet, and an exit state that happens to equal its
        // initial value would otherwise stop the walk before it started.
        auto changed = walked.add(index);

        for(U32 p = 0; p < pathCount; p++) {
            if(out[p] == state[p]) continue;
            out[p] = state[p];
            changed = true;
        }

        if(changed) wakeSuccessors(index);
    });
}

/*
 * The steps themselves, over settled states - twice.
 *
 * The first round decides which paths are released conditionally, and the second emits. They cannot
 * be one round: a flag has to be written at *every* transition of a path, including the ones in
 * blocks the walk reaches before the block whose conditional drop is the reason the flag exists.
 */
void Analysis::placeDrops() {
    if(!pathCount) return;

    for(auto walk: { WalkMode::Discover, WalkMode::Emit }) {
        for(U32 index = 0; index < blockCount; index++) {
            if(!reached.get(index)) continue;

            SmallArray<PathState, 64> state;
            auto in = stateAt(stateIn, index);
            for(U32 p = 0; p < pathCount; p++) state.push(in[p]);

            walkBlock(index, state.pointer(), int(walk));
        }

        /*
         * A flagged path's flag starts false, and a flagged *parameter*'s starts true.
         *
         * At the function's entry rather than at the first block's first position, because a
         * parameter's storage was filled by the call and there is no node in the body to attach the
         * fact to - which is the same reason its state starts `Owned` above.
         */
        if(walk != WalkMode::Discover) continue;

        for(U32 p = 0; p < pathCount; p++) {
            if(!out.flagged.get(base, p)) continue;

            auto place = body.place(base, path(MovePathId(p)).place);
            auto owned = place->kind == Place::Local &&
                         body.local(base, LocalId(place->payload))->isParameter();

            out.flags.push(program.mir, FlagStep { 0, FlagStep::kEntry, MovePathId(p), owned });
        }
    }
}

bool analyzeBody(Context& context, Program& program, Function& function) {
    if(!function.mir) return true;

    auto base = *program.mir;
    auto& body = *base[function.mir];
    assertTrue(body.sealed);

    auto elaboration = new (program.mir) Elaboration();
    body.elaboration = elaboration - base;

    Analysis analysis {
        context, program, function, body, base, *program.decl, *elaboration
    };

    /*
     * The order, and every step of it is a dependency rather than a preference.
     *
     * The origin environment comes first because nothing it reads is an analysis result - a
     * `Forward` operand never took a loan, so it does not have to be settled yet - and because
     * everything after it reads the environment. Extents follow it, liveness follows them (a loan
     * keeps the storage it came out of alive), `Forward` follows liveness, and initialization and
     * drop placement follow the moves that resolves.
     *
     * `checkLoans` runs where it does for one reason: it reports what the *program* got wrong about
     * borrows, and a body that failed it has nothing useful to say about initialization - every
     * value reached through a conflicting borrow would be reported again by the walk below.
     */
    analysis.buildPaths();
    analysis.computeOrigins();
    analysis.buildOriginLog();
    analysis.computeLoanExtents();
    analysis.publishOrigins();

    analysis.computeLiveness();
    if(!analysis.resolveForwards()) return false;
    if(!analysis.checkLoans()) return false;

    analysis.computeInitialization();
    analysis.placeDrops();
    return analysis.errors == 0;
}

bool analyzeProgram(Context& context, Program& program) {
    auto decl = *program.decl;
    auto analyzed = true;

    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];
        for(auto functionPointer: module->functions.contents(decl)) {
            /*
             * A specialization shares the generic's body and therefore its elaboration.
             *
             * This is where Implementation-Generics.md §1's invariant stops being a rule anyone has
             * to keep: drops are placed once, on the body the resolver built, and a specialization
             * cannot have a second placement because there is no second body to place one in.
             * Running this again would overwrite one answer with an identical one and would be the
             * beginning of two.
             */
            auto& function = *decl[functionPointer];
            if(function.specializes) continue;

            analyzed &= analyzeBody(context, program, function);
        }
    }

    return analyzed;
}

} // namespace mir
