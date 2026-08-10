#include "opt_pass.h"

/*
 * The three things a pass asks about the shape of a function rather than about one instruction:
 * which blocks dominate which, which blocks form a loop, and which locals a callee could reach.
 *
 * All three were written where they were first needed and all three are now wanted twice - dominance
 * by the common-subexpression pass and the loop pass, containment by place forwarding and the loop
 * pass - so they live here, and the file each of them used to be in says what it does with the
 * answer rather than how the answer is computed.
 *
 * Dominance is the textbook set fixpoint rather than the near-linear algorithm, on the same grounds
 * codegen/js/flow.cpp gives for its copy: a function's blocks are counted in tens, and being
 * obviously correct matters more than being fast when the failure mode is a value used where it has
 * not been computed.
 */

void computeDominance(OptContext& opt, Dominance& result) {
    auto& blocks = opt.function->blocks;
    auto count = blocks.size();

    /*
     * Emptied rather than built, on every field.
     *
     * `result` is `opt.dominance`, which the stage keeps for the length of the program: the rows,
     * the child lists and the three index arrays all still hold the last function's storage, and
     * `clear` on a Tritium array keeps the buffer it had. This is one of the two places the
     * optimizer used to allocate per block per round.
     */
    result.dominators.reset(count, count);
    result.blocks.clear();
    result.immediate.clear();
    result.preorder.clear();

    result.children.reset(count);

    for(Size i = 0; i < count; i++) {
        result.blocks.push(blocks.get(opt.local, i));
        result.immediate.push(Dominance::kNone);

        // Everything dominates everything until the fixpoint says otherwise, except at the entry
        // block, which only ever dominates itself.
        if(i == 0) result.dominators[0].set(0, true);
        else result.dominators[i].fill();
    }

    // Borrowed once rather than per block per round. The constructor zeroes it, and every block with
    // a predecessor overwrites the whole set with `copyFrom` before reading it - so the only case
    // that needed the zeroing is the one below that never reaches `copyFrom` at all.
    ScratchSet merged(opt.sets, count);

    auto changed = true;
    while(changed) {
        changed = false;

        for(Size i = 1; i < count; i++) {
            auto block = opt.local[result.blocks[i]];
            auto first = true;

            for(auto predecessor: block->incoming(opt.local)) {
                auto from = opt.local[predecessor]->index;

                if(first) {
                    merged->copyFrom(result.dominators[from]);
                    first = false;
                } else {
                    merged->intersectWith(result.dominators[from]);
                }
            }

            // A block nothing reaches is dominated by itself and nothing else, which keeps it out of
            // every scope below without needing a case of its own.
            if(first) merged->reset(count);
            merged->set(i, true);

            if(!merged->equals(result.dominators[i])) {
                result.dominators[i].copyFrom(*merged);
                changed = true;
            }
        }
    }

    /*
     * The immediate dominator is the one strict dominator that every other strict dominator also
     * dominates, which is the same as the one with the most dominators of its own.
     *
     * "How many dominators does block j have" is a property of j alone, so it is counted once per
     * block rather than once per pair. It used to be the innermost of three nested loops over the
     * block count, which made working out the immediate dominators cubic in the size of a function -
     * and this pass runs on every function on every optimizer round.
     */
    // Inline: it lives for this call, and sixty-four covers a function's blocks - see
    // compiler/util/README.md. A field on Dominance would look tidier and is not, because the two
    // callers do not share one.
    SmallArray<U32, 64> dominatorCount;
    for(Size j = 0; j < count; j++) dominatorCount.push(U32(result.dominators[j].popCount()));

    /*
     * Both walks go over the set rather than over the block range.
     *
     * "Is j in this row" asked once per block per block is a bit test each, and it costs the same
     * for a block with one dominator as for a block with all of them - which for the entry block's
     * successors is the usual case. Asking the row for its members instead makes each of these
     * linear in the answer, and the two of them together were a fifth of this function.
     */
    for(Size i = 1; i < count; i++) {
        auto best = Dominance::kNone;
        Size bestCount = 0;

        result.dominators[i].forEach([&](Size j) {
            if(i == j) return;

            Size own = dominatorCount[j];
            if(best != Dominance::kNone && own <= bestCount) return;

            best = U32(j);
            bestCount = own;
        });

        result.immediate[i] = best;
        if(best != Dominance::kNone) result.children[best].push(U32(i));
    }

    // A visit order in which a block comes before everything it dominates, which is what a pass that
    // moves an instruction needs: two values with a dependency between them are in dominance order
    // by construction, so laying them out in this order lays the definition out before the use.
    result.preorder.reserve(U32(count));
    for(Size i = 0; i < count; i++) result.preorder.push(0);

    SmallArray<U32, 32> stack;
    if(count) stack.push(0);

    U32 next = 0;
    while(stack.size()) {
        auto index = stack.pop().unwrap();

        result.preorder[index] = next++;
        for(auto child: result.children[index]) stack.push(child);
    }
}

/*
 * The natural loops, innermost first.
 *
 * A back edge is an edge whose target dominates its source, and the loop it closes is the target
 * plus every block that reaches the source without leaving through the target. That definition is
 * the one worth having here because of what it guarantees: the header is the *only* way in, so a
 * block outside the loop that jumps to it is a place every iteration is preceded by.
 *
 * Irreducible flow - a cycle with two entries - simply produces no back edge and therefore no loop,
 * which is the right answer rather than a missing case. Nothing this front end emits is irreducible
 * anyway, since `while`, `for` and the guard forms all build a single header.
 *
 * Two back edges to one header are one loop. Sorting by size afterwards is what puts an inner loop
 * in front of the outer one containing it, so a hoist walks outwards a level per round rather than
 * having to know how deeply anything is nested.
 */
void computeLoops(OptContext& opt, Dominance& dominance, Array<Loop>& loops) {
    // Emptied rather than assumed empty, which is what `computeDominance` above already does with the
    // structure it fills. Both callers used to hand over a list built one line earlier; the one that
    // does now is `loopsOf`, whose whole point is that the list is the stage's and outlives the
    // function. `clear` destroys each entry and keeps the storage - see compiler/util/README.md -
    // so a loop's IndexSet is freed and the array itself is reused.
    loops.clear();

    auto count = dominance.blocks.size();

    // Emptied per back edge rather than built per back edge. Inline, because the walk backwards from
    // one back edge reaches the blocks of one loop - see compiler/util/README.md.
    SmallArray<U32, 32> pending;

    for(Size i = 0; i < count; i++) {
        auto block = opt.local[dominance.blocks[i]];

        for(auto successor: block->successors()) {
            if(!successor) continue;

            auto header = opt.local[successor]->index;

            // The entry block heads no loop: nothing branches into a function, so an edge back to it
            // would be a second entry and the body below would be the whole function.
            if(header == 0 || !dominance.dominators[i][header]) continue;

            auto existing = maxLimit<Size>;
            for(Size l = 0; l < loops.size(); l++) {
                if(loops[l].header == header) existing = l;
            }

            if(existing == maxLimit<Size>) {
                existing = loops.size();

                Loop fresh;
                fresh.header = header;
                fresh.contains.reset(count);
                fresh.contains.set(header, true);
                loops.push(::move(fresh));
            }

            // Backwards from the source of the back edge, stopping at the header - which is already
            // marked, so it needs no test of its own.
            auto& loop = loops[existing];
            pending.clear();
            if(loop.contains.add(i)) pending.push(U32(i));

            while(pending.size()) {
                auto index = pending.pop().unwrap();

                for(auto predecessor: opt.local[dominance.blocks[index]]->incoming(opt.local)) {
                    auto from = opt.local[predecessor]->index;
                    if(!loop.contains.add(from)) continue;

                    pending.push(from);
                }
            }
        }
    }

    for(auto& loop: loops) {
        for(U32 i = 0; i < count; i++) {
            if(loop.contains[i]) loop.blocks.push(i);
        }

        /*
         * The one block outside the loop that jumps straight to the header.
         *
         * Both halves matter. It has to be the *only* outside predecessor, or an instruction put
         * there would run on one way in and not the other; and it has to have the header as its
         * only successor, or the instruction would also run on a path that never enters the loop -
         * which for a pure value is a waste rather than a fault, but for a load is a read that the
         * program did not ask for.
         *
         * Where there is no such block the loop is simply left alone. Making one means splitting an
         * edge, which is CFG surgery with phis attached, and every loop this front end emits already
         * has one: `while` builds the test as a block of its own with a single `jmp` into it.
         */
        for(auto predecessor: opt.local[dominance.blocks[loop.header]]->incoming(opt.local)) {
            auto from = opt.local[predecessor]->index;
            if(loop.contains[from]) continue;

            if(loop.preheader != Loop::kNone) {
                loop.preheader = Loop::kNone;
                break;
            }

            auto block = opt.local[predecessor];
            auto single = block->soleSuccessor() == dominance.blocks[loop.header];
            loop.preheader = single ? from : Loop::kNone;

            if(loop.preheader == Loop::kNone) break;
        }
    }

    for(Size i = 1; i < loops.size(); i++) {
        for(Size j = i; j > 0 && loops[j].blocks.size() < loops[j - 1].blocks.size(); j--) {
            swap(loops[j], loops[j - 1]);
        }
    }
}

/*
 * Whether a call that is handed this storage keeps a way to it after returning.
 *
 * The question `retained` was computed to answer, asked from the call site - see ArgSummary, and
 * analyze_escape.cpp, which asks the same thing of the same flag one stage earlier. A parameter is
 * retained when something derived from it outlives the call: stored into a global, captured by a
 * closure, written into another argument. An unretained one is reachable *while the callee runs* and
 * by nothing afterwards, because a call is synchronous and the borrow check has already refused a
 * body that would keep the address.
 *
 * `returnRoot` has to be tested beside it, because the two divide one fact between them. A body that
 * hands a borrow of its argument back is deliberately *not* marked retained - deriveSummary says why
 * at length, and would otherwise put every value anyone borrows from on the heap - so the return-root
 * marker is where that case is recorded. Read from the declaration rather than from `actualRoots`,
 * which is the conservative direction: the check is that the actual group is a subset of the
 * declared one.
 *
 * Everything else answers yes. A `CallDyn` has no callee to have a summary at all, which is the same
 * blanket assumption analyze.cpp states for it; a callee that is opaque or has not been summarized
 * yet is one nothing is known about; and a position past the end of the summary is a signature the
 * two disagree about, which is not a disagreement to resolve in favour of the optimizer.
 */
static bool callKeepsStorage(OptContext& opt, Value& instruction, ModulePtr<Value> storage) {
    ModuleList<ModulePtr<Value>, false>* args;
    ModulePtr<Function> callee;

    switch(instruction.kind) {
        case Value::Call:
            args = &((InstCall&)instruction).args;
            callee = ((InstCall&)instruction).callee;
            break;
        case Value::GenCall:
            args = &((InstGenCall&)instruction).args;
            callee = ((InstGenCall&)instruction).callee;
            break;
        default:
            return true;
    }

    if(!callee) return true;

    auto& summary = opt.local[callee]->summary;
    if(!summary.ready || summary.opaque) return true;

    U16 index = 0;
    auto keeps = false;

    for(auto arg: args->contents(opt.local)) {
        if(arg == storage) {
            if(index >= summary.args.size()) return true;

            auto entry = summary.args.get(opt.local, index);
            if(entry.retained || entry.returnRoot) keeps = true;
        }

        index++;
    }

    return keeps;
}

/*
 * Whether a borrow of a local is an address that outlives the instruction that received it.
 *
 * The same question `callKeepsStorage` answers about a whole record handed on as an argument, asked
 * one level deeper: `borrow_mut %out` followed by `call push, %v25` is the address of a local
 * reaching a callee, and if the callee retains nothing then no address of that local exists anywhere
 * outside the call. Which is exactly the fact `computeContainment` reports.
 *
 * Every reader of the borrow has to be such a call, and "reader" is the whole use list rather than
 * the argument positions - a borrow stored into a record, returned, merged by a phi or handed to a
 * `CallDyn` is one whose address the frame now holds, and each of those is a use.
 *
 * **A borrow used as a place root is refused, and that is not a conservatism.** A read or a write
 * through the borrow is an access to the local's own storage, and the two passes that read
 * containment for pointer disambiguation - `mayAlias` in opt_loop.cpp and `surveyCandidate` in
 * opt_promote.cpp - answer such a place against a local-rooted one by saying they cannot meet. That
 * answer is true only while no borrow-rooted place in the function names this local, which is what
 * this refuses to admit. What is admitted is the borrow whose only life is the call it was taken for.
 */
static bool borrowEndsAtItsCalls(OptContext& opt, ModulePtr<Inst> pointer) {
    auto borrow = (ModulePtr<Value>)pointer;

    for(auto user: opt.local[borrow]->uses(opt.local)) {
        auto& reader = *opt.local[user];

        switch(reader.kind) {
            case Value::Call:
            case Value::GenCall:
                if(callKeepsStorage(opt, reader, borrow)) return false;
                break;
            default:
                return false;
        }
    }

    return true;
}

/*
 * Which locals a callee could not reach *for longer than one instruction*.
 *
 * The use list answers this almost exactly, for the reason opt_scalar.cpp gives at greater length:
 * every instruction naming a place rooted in a local is recorded as a user of that local's `Alloc`,
 * so a local whose users are all reads and writes *of its own place* has had its address handed to
 * nothing. It was not pointed at and it was not captured, because each of those is an instruction
 * and each would be in the list.
 *
 * Everything not admitted below declines. `Address` hands out the address that is the whole
 * question; `Drop` runs a teardown that receives one; `Move`, `Swap` and `Exchange` are ownership
 * transfers the analyses already decided and not something to reason about the storage of here.
 *
 * The alloc appearing as an *operand* rather than as a place root is the whole-aggregate read a call
 * argument is - `call g, %v2` hands the record itself on. opt_arg.cpp found the same case from the
 * other side, and it used to be the one way a local on the list above still escaped.
 *
 * **A call argument is exposure with an end to it**, which is the one thing this is not
 * flow-insensitive about. An unretained argument is reachable only while the callee runs, so it is
 * admitted here and the *call* forgets what it may have written instead - see `forgetExposed`'s
 * caller in opt_place.cpp and `scanEffects` in opt_loop.cpp, which are the two readers and both have
 * to do it. Without that split a record handed to `==` once was un-forwardable everywhere in the
 * function, including at instructions in front of the call: Default.yana built `Flags {read: True,
 * write: True}` and then branched on a load of the field it had just written.
 *
 * **And a borrow taken for a call is the same statement one level deeper** - see
 * `borrowEndsAtItsCalls`, which is where the rule and its one refusal live. This is the half that
 * reaches ordinary container code: `push(out, 0)` is a `borrow_mut` of a local, and without it every
 * loop that both writes through a container's elements and reads its header reloaded the header
 * every iteration, because the local a `Borrow` named was exposed for the rest of the frame.
 *
 * It costs two more readers than the argument case did, and the reason is the asymmetry
 * `eachAddressedLocal` states: a by-value argument is a binding the callee cannot assign, an address
 * is not. So `eliminateCommonValues` and `promotePlaces` - which forward across a by-value argument
 * on purpose - have to end their facts at a call that was handed an address, and the second declines
 * such a local outright because what it builds is a value per block rather than a scope to forget in.
 *
 * Otherwise flow-insensitive on purpose: a local whose address is held by a *value* - anything
 * `Address` produced, or a borrow that outlives its call - is treated as reachable everywhere, which
 * costs the forwarding before that instruction and needs no reasoning about where a loan ends.
 */
void computeContainment(OptContext& opt, IndexSet& contained) {
    contained.reset(opt.function->localCount());

    for(U32 i = 0; i < opt.function->localCount(); i++) {
        auto slot = opt.function->localAt(opt.local, i);

        /*
         * A `&` parameter's slot is the caller's storage, and is not contained by this frame
         * whatever this frame does.
         *
         * A *closure environment* used to be refused beside it on the reading that the storage is
         * the function value's. That is a statement about who frees it - see Local::closureEnv and
         * eliminateDeadLocal, which still asks it - and containment is a question about who can
         * *reach* it, which the walk below already answers correctly and more precisely. A live
         * closure reaches its captures through the environment word, and that word is an `addressof`
         * of this local: `Value::Address` is not in the switch below, so a closure anything can still
         * call is refused there and the flag adds nothing.
         *
         * What it did add was the case where the address is gone. A `calldyn` this stage resolved
         * takes the function value with it, and the environment left behind is an ordinary record of
         * this frame's that nothing outside can name - which is exactly the storage the captures
         * have to be forwarded out of for the *next* call to resolve. See inlineDynamicCall.
         */
        auto ok = slot.value && opt.local[slot.value]->kind == Value::Alloc && !slot.borrowed;

        if(ok) {
            for(auto user: opt.local[slot.value]->uses(opt.local)) {
                auto& instruction = *opt.local[user];
                auto handedOver = false;

                switch(instruction.kind) {
                    case Value::Init:
                    case Value::Assign:
                    case Value::LoadPlace:
                    case Value::Copy:
                    // The writes a construction is, said once - see InstAggregate. Admitted for the
                    // same reason the two stores are: it names a place rooted here and hands the
                    // address to nothing. The `eachOperand` test below still catches this local
                    // appearing as one of the *values*, which is the whole-aggregate read.
                    case Value::Aggregate:
                        break;

                    // The storage passed on as an argument, which is exposure for the length of the
                    // call and no longer - when the callee's summary says so.
                    case Value::Call:
                    case Value::GenCall:
                        handedOver = true;
                        ok = !callKeepsStorage(opt, instruction, slot.value);
                        break;

                    // The address of the storage, which is the same statement when every reader of
                    // that address is such a call - see `borrowEndsAtItsCalls`. The borrow itself is
                    // not an operand of anything here, so there is nothing for the test below to
                    // add: what it hands out is its own result.
                    case Value::Borrow:
                        handedOver = true;
                        ok = borrowEndsAtItsCalls(opt, user);
                        break;

                    default:
                        ok = false;
                }

                if(!ok) break;
                if(handedOver) continue;

                eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
                    if(operand == slot.value) ok = false;
                });

                if(!ok) break;
            }
        }

        contained.set(i, ok);
    }
}

// The blocks control can actually get to. Needed because the analysis below starts from the
// optimistic end, and a cycle nothing enters would otherwise convince itself that it holds whatever
// it likes - there being no path into it to contradict the claim.
void computeReachable(OptContext& opt, IndexSet& reachable) {
    auto count = opt.function->blocks.size();
    reachable.reset(count);
    if(!count) return;

    SmallArray<U32, 32> pending;
    reachable.set(0, true);
    pending.push(0);

    while(pending.size()) {
        auto index = pending.pop().unwrap();

        for(auto successor: opt.local[opt.function->blocks.get(opt.local, index)]->successors()) {
            if(!successor) continue;

            auto to = opt.local[successor]->index;
            if(!reachable.add(to)) continue;

            pending.push(to);
        }
    }
}

bool staysInFrame(OptContext& opt, const IndexSet& contained, const Place& place) {
    if(place.root != PlaceRoot::Local) return false;
    if(!contained[place.local]) return false;

    // `get` is a read that the list spells as a mutation, which is why every walk of a projection
    // path in this directory casts the constness off rather than taking a copy of the path.
    auto& projections = const_cast<Place&>(place).projections;

    for(Size i = 0; i < projections.size(); i++) {
        switch(projections.get(opt.local, i).kind) {
            case ProjectionKind::Field:
            case ProjectionKind::Downcast:
            case ProjectionKind::Discriminant:
            case ProjectionKind::Unit:
                break;
            default:
                return false;
        }
    }

    return true;
}

/*
 * The cached forms - see AnalysisStamp, which is where the argument for them lives.
 *
 * Each is the same two lines: hand back what is there if the stamp still holds, otherwise recompute
 * and stamp it. The buffers are the stage's, so a recompute reuses whatever the last function left
 * allocated and only the answer is rebuilt.
 */
Dominance& dominanceOf(OptContext& opt) {
    if(!opt.dominanceStamp.holds(opt.function, opt.version, false)) {
        computeDominance(opt, opt.dominance);
        opt.dominanceStamp.take(opt.function, opt.version);
    }

    return opt.dominance;
}

Array<Loop>& loopsOf(OptContext& opt) {
    if(!opt.loopStamp.holds(opt.function, opt.version, false)) {
        // Through the cached tree rather than a fresh one: the loops are derived from it, so a
        // reading of the two that disagreed would be a loop whose header does not dominate it.
        computeLoops(opt, dominanceOf(opt), opt.loops);
        opt.loopStamp.take(opt.function, opt.version);
    }

    return opt.loops;
}

/*
 * Over the values as well as the blocks, which is what the `true` says: what this reads is the use
 * list of every local's `Alloc`, so an instruction removed anywhere in the function can be the one
 * that was holding a local's address.
 *
 * The local count is compared as well, and that is not redundant with the version. A pass that adds
 * a local - `scalarizeLocals` gives each field of a taken-apart record one - does so through the
 * function rather than through the editor, and every such pass rewrites instructions too, so the
 * version has always moved by the time it matters. The comparison is what makes that an argument
 * about this file rather than about the four passes that add locals.
 */
const IndexSet& containmentOf(OptContext& opt) {
    auto stale = !opt.containedStamp.holds(opt.function, opt.version, true)
              || opt.contained.size() != opt.function->localCount();

    if(stale) {
        computeContainment(opt, opt.contained);
        opt.containedStamp.take(opt.function, opt.version);
    }

    return opt.contained;
}

const IndexSet& reachableOf(OptContext& opt) {
    auto stale = !opt.reachableStamp.holds(opt.function, opt.version, false)
              || opt.reachable.size() != opt.function->blocks.size();

    if(stale) {
        computeReachable(opt, opt.reachable);
        opt.reachableStamp.take(opt.function, opt.version);
    }

    return opt.reachable;
}
