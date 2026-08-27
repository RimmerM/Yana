#include "opt_pass.h"

/*
 * Cross-block place forwarding: a read of storage answered by the value written into it, wherever in
 * the function that write happened.
 *
 * opt_place.cpp answers the same question within one block and stops at the boundary, which is the
 * half that costs nothing. This is the other half, and it is a different pass rather than a longer
 * version of that one for two reasons: what the answer needs at a join is a *value* that does not
 * exist yet - the phi - and what entitles it to give one at all is a proof about the storage rather
 * than a walk of the instructions.
 *
 * ## The proof
 *
 * Everything here rests on `computeContainment`: a local whose `Alloc` is used only by reads and
 * writes of its own place has had its address handed to nothing, so no pointer and no capture can
 * reach it. Which means the only places that can overlap one of its fields are other places rooted
 * in the same local. That is what removes the aliasing question entirely, and it is why this needs no
 * clobber rule and no notion of a barrier - a call in the middle of the function is not an event at
 * all here.
 *
 * With the one exception that containment is now weaker than that sentence: a local handed to a call
 * that retains nothing is contained, and so is one a `Borrow` taken for such a call names. Neither
 * leaves an address behind afterwards, which is all the aliasing rule needed, and both may be
 * *written* while the call runs. `surveyCandidate` declines such a local rather than modelling it -
 * see the note there, which is why this is not a barrier after all.
 *
 * The reasoning is opt_scalar.cpp's, one step further: that pass uses the use list to prove a local
 * is *written and never read*, and removes it. This one uses the same list to prove a local is
 * private, and removes the reads so that the other pass can then see it that way. Which is the point
 * of the pair - neither turns a constructed record back into values on its own.
 *
 * ## The algorithm
 *
 * lower_promote.cpp's, over places instead of allocas. Rather than compute an iterated dominance
 * frontier it reads what a block carries in off its predecessors, which the block list being in
 * reverse postorder is what makes possible: one predecessor, or several that agree, is that value
 * directly, and only a disagreement or a predecessor not yet visited - a back edge, so a loop header
 * - is a phi. "Known to arrive written" is a plain forward AND-dataflow that has to be computed
 * anyway - it is what decides that every alternative of a phi exists, and that no read is of storage
 * nothing ever put anything in. It is solved from the *optimistic* end, which is the one place this
 * departs from that file and the reason it can do anything about a loop at all - see
 * `computeAvailability`.
 *
 * Both files used to place a phi in *every* block the storage arrives already written into and let
 * the trivial-phi sweep take back the ones whose alternatives agreed. That is the same answer by a
 * route whose cost is the number of candidates times the number of blocks, and on an inlined body
 * with 480 candidates over 1,801 blocks it was 437,360 phis to reach a few hundred - see the note
 * above the placement below.
 *
 * A read of a place that has *not* been written on every path is not merely unforwarded: it makes the
 * place unpromotable altogether, because a phi placed above it would have an edge with no value to
 * offer. In a well-formed program it should not arise - the resolver initializes before it reads, and
 * the demand analysis says so - but a place is finer-grained than a local, and it is cheaper to
 * decline than to prove that it never happens.
 *
 * ## What this does not touch
 *
 * The writes. Every one of them stays exactly where it was, and that is not a limitation but the
 * rule: `Init` and `Assign` are the ownership decisions the analyses already took, and a pass that
 * removed one would be deciding that a value's storage is not where the drop pass believes it is.
 * What makes the record actually disappear is opt_scalar.cpp afterwards, which removes a local whose
 * whole use list is writes - and the reads this pass removed are what put it in that state.
 *
 * So the *measurable* effect of this pass is entirely in what runs after it, which is worth stating
 * because the .opt.expect dumps show it that way: the loads vanish here, and the allocation and its
 * stores vanish on the next round.
 */

namespace {

/*
 * One piece of storage this pass is trying to hold in values, and the dataflow that says where.
 *
 * `entry` is the phi carrying the place into a block, where one was placed, and `exit` what the block
 * leaves it holding. The two are separate arrays rather than a walk, because the alternatives of a
 * phi are what its predecessors end up holding and none of those is known until every block has been
 * rewritten - so the phis are built detached and only reach their blocks at the end.
 */
struct Candidate {
    Place place;
    TypePtr type = nullptr;

    // Which column of the per-block availability table answers for this place - see `surveyPlaces`,
    // which holds one row per block over the numbering the candidates had before any were dropped.
    U32 column = 0;

    ValueList entry;
    ValueList exit;

    // The first read, which is where the constant below is blamed on and what its type came from.
    ModulePtr<Value> read = nullptr;

    // Set for a place nothing in the function ever writes - see `fillUnwritten`. The value every
    // read of it is answered with, and there is no dataflow left to do once there is one.
    ModulePtr<Value> initial = nullptr;

    // Whether any block writes the place. The same fact as a non-empty `stores`, recorded while the
    // survey is walking rather than scanned back out of the set afterwards.
    bool written = false;
};

/*
 * One phi this pass placed, the block it belongs to and the candidate it carries.
 *
 * Kept beside the phi rather than looked back out of `Candidate::entry`, because that array no longer
 * names a phi everywhere it holds a value: a block whose predecessors all hand it the same thing
 * carries that value in directly and has nothing to fill in. So the fill below walks the phis that
 * were placed instead of the blocks that have a value, and `removeTrivialPhis` walks the same list.
 */
struct PlacedPhi {
    ModulePtr<InstPhi> phi = nullptr;
    ModulePtr<Block> block = nullptr;
    U32 candidate = 0;
};

bool isWrite(const Value& instruction) {
    return instruction.kind == Value::Init || instruction.kind == Value::Assign;
}

// Whether an instruction's place slots are reads. Containment has already restricted a contained
// local's users to these four kinds, so everything else is a kind whose effect on storage this pass
// has not checked - and an unchecked one disqualifies rather than being assumed harmless.
bool isRead(const Value& instruction) {
    return instruction.kind == Value::LoadPlace || instruction.kind == Value::Copy;
}

/*
 * Every place an instruction writes, with the value it puts there.
 *
 * One for a store and one per component for an aggregate - see InstAggregate, whose components are
 * the stores it replaced. It exists because `eachPlace` cannot answer this: an aggregate reports its
 * place with *no* projection, which is the right prefix for a kill and the wrong place for a write,
 * and there is no bound on how many components one has to report through a fixed array.
 *
 * Nothing for every other kind, which is what makes a read a question for `eachPlace` and a write a
 * question for this one.
 */
template<class F>
void eachWrittenPlace(OptContext& opt, Value& instruction, F&& f) {
    if(isWrite(instruction)) {
        auto& store = (InstInit&)instruction;
        f(store.place, store.value);
        return;
    }

    if(instruction.kind != Value::Aggregate) return;

    eachWrittenComponent(opt.local, opt.module->arena, (InstAggregate&)instruction,
                         [&](Place place, ModulePtr<Value> value, Size) { f(place, value); });
}

/*
 * The candidates rooted in each local, as a chain per local.
 *
 * Every question this pass asks of a place - is it one of the candidates, does it overlap one, does
 * an instruction reach one - is a question only about candidates rooted in the *same local*, because
 * a place rooted in a different one is different storage by construction and containment has already
 * ruled out anything that could alias across it. So the list to walk is that local's, and a scan of
 * all of them is a scan of everything that cannot possibly match.
 *
 * That scan is what the survey and the rewrite both used to do, once per place per instruction, with
 * a `samePlace` call at each step. On a body with hundreds of candidates it was the whole cost of
 * the pass, and it is a walk whose answer is settled before it starts.
 */
struct CandidateIndex {
    // Local -> the first candidate rooted in it, and one link per candidate after that. A chain
    // rather than a list per local because the number of locals grows with the body and the number
    // of candidates does not: most locals have none, and this allocates nothing for those.
    HashMap<U32, U32> first;
    Array<U32> next;

    void build(Array<Candidate>& candidates) {
        first.clear();
        next.clear();

        for(Size i = 0; i < candidates.size(); i++) {
            auto& place = candidates[i].place;
            next.push(maxLimit<U32>);

            if(place.root != PlaceRoot::Local) continue;

            auto found = first.getValue(place.local);
            if(found) next[i] = found.unwrap();

            first.add(place.local, U32(i));
        }
    }

    // Every candidate rooted in this local, most recently added first.
    template<class F>
    void eachIn(U32 local, F&& f) const {
        auto found = first.getValue(local);
        if(!found) return;

        for(auto at = found.unwrap(); at != maxLimit<U32>; at = next[at]) f(Size(at));
    }
};

Size candidateOf(OptContext& opt, const CandidateIndex& byLocal, Array<Candidate>& candidates,
                 const Place& place) {
    if(place.root != PlaceRoot::Local) return maxLimit<Size>;

    auto answer = maxLimit<Size>;
    byLocal.eachIn(place.local, [&](Size i) {
        if(answer == maxLimit<Size> && samePlace(opt, candidates[i].place, place)) answer = i;
    });

    return answer;
}

/*
 * The places worth trying, which are the ones something reads *inside* an aggregate.
 *
 * A place nothing reads has nothing to forward, and a place whose reads are all in one block was
 * already answered by opt_place.cpp - but that one is admitted anyway rather than filtered out,
 * because deciding it costs a walk and doing it costs a phi that immediately proves trivial.
 *
 * A place with no projections is declined, and that one is a real rule rather than a shortcut. Such
 * a place is a whole local of scalar type, which is storage *both* targets already have a better
 * form for and reach without any help: natively `promoteStackSlots` turns the alloca into registers
 * with phis of its own, and on JS a scalar local was never memory at all - it is a `var`, read and
 * assigned in place.
 *
 * Promoting one here therefore buys nothing and costs something, because the phi form is worse than
 * what is already there on the target that cannot undo it: codegen/js emits a phi as a fresh
 * variable plus a copy on every edge, with no coalescing to notice that `total` and `total + 12` can
 * share one. A loop accumulator measured as three statements where it had been one.
 *
 * What is left after the rule is exactly the storage neither target can flatten for itself - a field
 * of a record, a payload behind a downcast, a word two packed fields share - and taking the reads of
 * one away is what lets opt_scalar.cpp remove the record. That is the whole point of the pass, and
 * the case this declines was never part of it.
 */
void collectCandidates(OptContext& opt, const IndexSet& contained, Array<Candidate>& into,
                       CandidateIndex& byLocal) {
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        for(auto pointer: opt.local[blockPointer]->instructions(opt.local)) {
            auto instruction = opt.local[pointer];
            if(instruction->kind != Value::LoadPlace) continue;

            auto& load = (InstLoadPlace&)*instruction;
            if(load.place.projections.isEmpty()) continue;
            if(!holdsLoadableValue(opt, load.type)) continue;
            if(!staysInFrame(opt, contained, load.place)) continue;
            if(candidateOf(opt, byLocal, into, load.place) != maxLimit<Size>) continue;

            Candidate candidate;
            candidate.place = load.place;
            candidate.type = load.type;
            candidate.read = (ModulePtr<Value>)pointer;
            into.push(::move(candidate));

            // Linked in as it is added, so that the duplicate test above stays a walk of this
            // local's candidates rather than of every candidate found so far.
            byLocal.next.push(maxLimit<U32>);

            auto head = byLocal.first.getValue(load.place.local);
            if(head) byLocal.next[into.size() - 1] = head.unwrap();

            byLocal.first.add(load.place.local, U32(into.size() - 1));
        }
    }
}

/*
 * The whole survey, over one walk of the function rather than one walk per candidate.
 *
 * Three answers used to be computed per candidate, and each of them costs a walk of something the
 * size of the function: the blocks that write the place, the availability fixpoint over the CFG, and
 * the check that nothing reads the place before anything wrote it. A body with a candidate per field
 * therefore paid the function's size twice over, and an inlined test case of 32,000 instructions
 * with 480 candidates spent 0.9s here. Held per *block* instead - one bit per candidate in a row per
 * block - all three fall out of a single pass, and the dataflow moves a word of candidates at a time
 * rather than a set per candidate.
 *
 * ## What can reach the storage
 *
 * "Anything else" is a much shorter list than it would be for arbitrary storage, and that is the
 * whole value of the containment proof: a place rooted in a pointer or a borrow cannot name a
 * contained local, a call cannot reach one, and a place rooted in a *different* local is different
 * storage by construction. So the only thing that can disturb a place is another access rooted in
 * the same local, and the only one that disturbs it is a write to an overlapping path - which is why
 * the walk below asks `byLocal` for the candidates to consider rather than considering them all.
 *
 * The type has to be the same at every access for the same reason a phi's alternatives do: what
 * replaces the reads is one value, and one value has one type. Reading a place at two types is not
 * something the resolver emits - a projection has the field's type - but a `Unit` path introduced by
 * the packing expansion is a reading of storage rather than of a field, and this is what would
 * notice if two of those ever disagreed.
 *
 * ## Where the place is known to hold something
 *
 * A forward must-analysis - `in[b] = AND over predecessors of out[p]`, `out[b] = in[b] || writes` -
 * and the greatest fixpoint of it rather than the least, which is the one distinction in this file
 * that changes what the pass can do rather than how fast it does it.
 *
 * Starting every block at "written" and letting the iteration take that away is what lets a place
 * written *before* a loop be available *inside* it. The other direction cannot: a header's
 * availability depends on the back edge, the back edge's on the header, and from the pessimistic end
 * that circle has nothing to break it, so the answer converges to "unwritten" for a place the loop
 * merely reads. Which is precisely the case worth having - a record built before a loop and read in
 * its body is what an inlined constructor leaves behind.
 *
 * It is still a *must* answer for every block control can reach, because the entry starts at what it
 * writes and nothing else has a way in. A cycle nothing reaches is the one place the optimistic start
 * would assert something unfounded, and it is excluded rather than reasoned about.
 *
 * An unreachable *predecessor* of a reachable block still denies it a phi, since its `out` stays
 * zero. That is the conservative answer and the convenient one: an alternative arriving over an edge
 * nothing takes is still an alternative the phi would have to name.
 *
 * ## And the reads
 *
 * Whether every read of a place follows something that wrote it. The one thing that can still
 * disqualify a place at this point, and it disqualifies it completely: a value cannot carry what was
 * never put in it. Rejecting one candidate says nothing about any other - every question here is
 * asked of one place, and a read of a rejected one is an ordinary read rather than an event in
 * another's dataflow - so nothing is recomputed afterwards.
 */
void surveyPlaces(OptContext& opt, Array<Candidate>& candidates, const CandidateIndex& byLocal,
                  const IndexSet& reachable, IndexSetList& available, IndexSet& usable,
                  IndexSet& readsAreWritten) {
    auto blockCount = opt.function->blocks.size();
    auto count = candidates.size();

    usable.reset(count);
    usable.fill();

    // Kept apart from `usable`, because the two failures have different answers: a place read before
    // anything wrote it may still be one nothing writes at all, and `fillUnwritten` has a value for
    // that. A place the survey itself declined has none.
    readsAreWritten.reset(count);
    readsAreWritten.fill();

    IndexSetList stores;
    stores.reset(blockCount, count);

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(auto pointer: block->instructions(opt.local)) {
            auto instruction = opt.local[pointer];
            auto written = isWrite(*instruction) || instruction->kind == Value::Aggregate;

            auto visit = [&](const Place& place, ModulePtr<Value> stored) {
                if(place.root != PlaceRoot::Local) return;

                byLocal.eachIn(place.local, [&](Size which) {
                    auto& candidate = candidates[which];

                    if(samePlace(opt, place, candidate.place)) {
                        if(!written) {
                            // One value replaces every read, and one value has one type - so a read
                            // of this storage at another type is not something to answer, whatever
                            // produced it. See `collectCandidates`, which took the type from the
                            // first one.
                            if(instruction->kind == Value::LoadPlace &&
                               instruction->type != candidate.type) usable.set(which, false);

                            return;
                        }

                        if(opt.local[stored]->type != candidate.type) usable.set(which, false);

                        stores[block->index].set(which, true);
                        candidate.written = true;
                        return;
                    }

                    if(!pathsMayOverlap(opt, place, candidate.place)) return;
                    if(!isRead(*instruction)) { usable.set(which, false); return; }

                    /*
                     * A read of an aggregate is the one read that may not be one.
                     *
                     * `LoadPlace` of a memory type does not answer with the storage's contents -
                     * there is no value of that shape to answer with - so what its result stands for
                     * is the storage itself, and anything that consumes one may be writing through
                     * it. The resolver does not build that shape today: an aggregate handed to a
                     * call or written whole is the `Alloc` itself as an operand, which containment
                     * already declines, and the whole-record load in front of every field access is
                     * read by nothing and removed by `isDeadRead`. So this costs nothing now and is
                     * what would notice if that ever stopped being true.
                     */
                    if(instruction->kind != Value::LoadPlace) return;
                    if(!holdsLoadableValue(opt, instruction->type) && instruction->useCount() != 0) {
                        usable.set(which, false);
                    }
                });
            };

            /*
             * The writes an aggregate is, in place of the whole-value place it reports.
             *
             * Its own place has an empty path, so leaving it to `eachPlace` would make every
             * construction overlap every candidate inside it and disqualify the lot - which is what
             * cost the native build eighty stores when records first became one instruction.
             */
            if(instruction->kind == Value::Aggregate) {
                eachWrittenPlace(opt, *instruction, visit);
            } else {
                auto stored = isWrite(*instruction) ? ((InstInit*)instruction)->value : nullptr;
                eachPlace(*instruction, [&](const Place& place) { visit(place, stored); });
            }

            /*
             * And the instruction that was handed the storage itself, which is the one write this
             * walk cannot see and cannot answer.
             *
             * `computeContainment` admits an unretained call argument and an unretained borrow of a
             * local: neither leaves an address behind, which is what entitles the aliasing rule
             * above to say that only a place rooted in this same local can reach this one. What it
             * does not say is that the callee wrote nothing *while it ran*, and a write it made is a
             * store this pass would have to have a value for.
             *
             * The other three readers of containment forget at the call and carry on. This one has
             * nothing to forget *to* - what it is building is a value per block for the whole
             * function - so the candidate is declined outright. It is the storage a callee received,
             * which is exactly the storage this pass has no business holding in registers.
             */
            eachAddressedLocal(opt, *instruction, [&](U32 local) {
                byLocal.eachIn(local, [&](Size which) { usable.set(which, false); });
            });
        }
    }

    IndexSetList out;
    available.reset(blockCount, count);
    out.reset(blockCount, count);

    for(Size i = 0; i < blockCount; i++) {
        if(reachable[i]) out[i].fill();
    }

    IndexSet incoming;
    incoming.reset(count);

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];
            if(!reachable[block->index]) continue;

            auto firstEdge = true;
            for(auto predecessor: block->incoming(opt.local)) {
                auto& row = out[opt.local[predecessor]->index];

                if(firstEdge) { incoming.copyFrom(row); firstEdge = false; }
                else incoming.intersectWith(row);
            }

            if(firstEdge) incoming.reset(count);

            if(!available[block->index].equals(incoming)) {
                available[block->index].copyFrom(incoming);
                changed = true;
            }

            incoming.unionWith(stores[block->index]);

            if(!out[block->index].equals(incoming)) {
                out[block->index].copyFrom(incoming);
                changed = true;
            }
        }
    }

    IndexSet written;

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];
        if(!reachable[block->index]) continue;

        written.copyFrom(available[block->index]);

        for(auto pointer: block->instructions(opt.local)) {
            auto instruction = opt.local[pointer];

            if(instruction->kind == Value::Aggregate) {
                eachWrittenPlace(opt, *instruction, [&](const Place& place, ModulePtr<Value>) {
                    auto which = candidateOf(opt, byLocal, candidates, place);
                    if(which != maxLimit<Size>) written.set(which, true);
                });
            } else {
                eachPlace(*instruction, [&](const Place& place) {
                    auto which = candidateOf(opt, byLocal, candidates, place);
                    if(which == maxLimit<Size>) return;

                    if(isWrite(*instruction)) written.set(which, true);
                    else if(!written[which]) readsAreWritten.set(which, false);
                });
            }
        }
    }
}

/*
 * A place **nothing in the function writes**, answered with zero.
 *
 * `readsAreWritten` declines a place read before anything put something in it, and its comment says
 * why that used to be a case that should not arise: the resolver initializes before it reads. It
 * arises now, and deliberately - `splitPhiOfLocals` in opt_scalar.cpp reads every place the copy it
 * removes was read at, off *each* alternative, and an alternative that is the absent constructor of a
 * sum has nothing in its payload. `Maybe(Int)`'s `Nothing` arm writes the discriminant and stops, so
 * a payload read of it is a read of an allocation nobody ever filled in.
 *
 * The value of such a read is whatever the allocation happened to contain, and the entitlement to say
 * "zero" instead comes from that being the whole list of possibilities. The survey above has already
 * proved every part of it:
 *
 *  - the place is inside a **contained** local, so no pointer, borrow or callee can reach it;
 *  - `surveyCandidate` refused any write to an *overlapping* path, so a whole-local write or an
 *    aggregate covering it would have declined the candidate rather than gone unnoticed;
 *  - and no block writes the place itself, which is what `written` records.
 *
 * So the storage holds what the allocation left there, unspecified on native and `zeroValue` on JS,
 * and zero is a legal reading of it on both. That is the same ruling `lower_split.cpp` takes one tier
 * down for a live cell no write reaches, and for the same reason: the alternative is not a better
 * value, it is keeping the allocation.
 *
 * **The local has to be one this frame allocated.** A parameter's storage is filled in by the caller
 * before the function starts, so a field of one that this body only reads is not empty at all - it is
 * the argument. That is the whole of what would go wrong here and it would go wrong silently:
 * `orZero(m: Maybe(Int))` reads `%m@Just` and writes it nowhere, so the mistake is not an edge case
 * but every function that takes a record apart. A parameter's slot holds an `Arg` rather than an
 * `Alloc`, which is the same test `eliminateDeadLocal` makes for the same reason.
 */
bool fillUnwritten(OptContext& opt, Candidate& candidate) {
    if(candidate.written || !candidate.read) return false;
    if(candidate.place.root != PlaceRoot::Local) return false;

    auto slot = opt.function->localAt(opt.local, candidate.place.local);
    if(!slot.value || opt.local[slot.value]->kind != Value::Alloc) return false;
    if(slot.borrowed || slot.closureEnv) return false;

    auto& at = *opt.local[candidate.read];

    // A type this stage has a zero of. Everything else declines rather than being guessed at: what
    // the constant has to be is a value the backends can spell, and the three below are the ones
    // `makeConstant` and `makeFloatConstant` are the spelling of.
    if(foldableInt(opt, candidate.type) || isConstructorIndex(opt, candidate.type)) {
        candidate.initial = makeConstant(opt, at, candidate.type, 0);
        return true;
    }

    if(foldableFloat(opt, candidate.type)) {
        candidate.initial = makeFloatConstant(opt, at, candidate.type, 0.0);
        return true;
    }

    return false;
}

/*
 * The rewrite: one pass over each block, answering every read from what the place is holding.
 *
 * The writes stay where they are, so this walks the list rather than rebuilding it - the only
 * instruction it removes is a load whose readers have all been pointed elsewhere.
 */
void rewriteBlock(OptContext& opt, Block& block, Array<Candidate>& candidates,
                  const CandidateIndex& byLocal) {
    ValueList current;
    for(auto& candidate: candidates) current.push(candidate.entry[block.index]);

    for(Size i = 0; i < block.instructionCount(); i++) {
        auto pointer = block.instructionAt(opt.local, i);
        auto instruction = opt.local[pointer];

        if(instruction->kind == Value::LoadPlace) {
            auto which = candidateOf(opt, byLocal, candidates,
                                     ((InstLoadPlace&)*instruction).place);
            if(which == maxLimit<Size>) continue;

            // Reading a place that holds nothing is what `readsAreWritten` declines a candidate for,
            // so arriving here with nothing in hand is a disagreement between the two rather than a
            // program this pass has to cope with.
            assertTrue(current[which] != nullptr);

            opt.ir().replaceValue((ModulePtr<Value>)pointer, current[which]);
            opt.ir().eraseInstruction(pointer);
            i--;
        } else {
            eachWrittenPlace(opt, *instruction, [&](const Place& place, ModulePtr<Value> stored) {
                auto which = candidateOf(opt, byLocal, candidates, place);
                if(which != maxLimit<Size>) current[which] = stored;
            });
        }
    }

    for(Size i = 0; i < candidates.size(); i++) candidates[i].exit[block.index] = current[i];
}

/*
 * The phis that say nothing, removed.
 *
 * Two shapes, and they are worth keeping apart. A phi nothing reads is deleted outright and is not a
 * change to the function. A phi whose alternatives all agree *is* a change, because something reads
 * it and now reads the value directly instead; a self-reference does not count as an alternative,
 * which is what collapses a loop-carried phi whose body never writes the place.
 *
 * A worklist rather than a sweep repeated until nothing changes. Removing a phi can only make two
 * other things removable - a phi that *read* it may now be trivial, and a phi it read may now have
 * no readers left - so those are what goes back on the list. The sweep this replaces rescanned every
 * surviving phi to find them, once per removal, and shifted the array down by one on top of that;
 * both are linear in the number of phis and both ran per phi.
 */
void removeTrivialPhis(OptContext& opt, Array<PlacedPhi>& placed) {
    // Which entry a phi is, so that one reached through a use list or an alternative can be put back
    // on the worklist. Only phis this pass placed are in it: any other phi's alternatives were
    // already whatever they were, and removing one of these cannot have changed them.
    HashMap<U32, U32> byPhi;
    for(Size i = 0; i < placed.size(); i++) byPhi.add(U32(placed[i].phi), U32(i));

    IndexSet gone;
    gone.reset(placed.size());

    Array<U32> work;
    for(Size i = placed.size(); i > 0; i--) work.push(U32(i - 1));

    while(work.size()) {
        auto at = Size(work.pop().unwrap());
        if(gone[at]) continue;

        auto pointer = placed[at].phi;
        auto phi = opt.local[pointer];

        // Everything the removal could disturb, collected before it does: the edit below moves both
        // the use list and the alternatives.
        SmallArray<U32, 8> woken;
        auto wake = [&](U32 value) {
            auto found = byPhi.getValue(value);
            if(found) woken.push(found.unwrap());
        };

        auto finish = [&]() {
            gone.set(at, true);
            for(auto index: woken) {
                if(!gone[index]) work.push(index);
            }
        };

        if(phi->useCount() == 0) {
            for(auto input: phi->inputs.contents(opt.local)) wake(U32(input.value));

            opt.ir().erasePhi(pointer);
            finish();
            continue;
        }

        ModulePtr<Value> only = nullptr;
        auto trivial = true;

        for(auto input: phi->inputs.contents(opt.local)) {
            if(input.value == (ModulePtr<Value>)pointer) continue;

            if(!only) only = input.value;
            else if(only != input.value) { trivial = false; break; }
        }

        if(!trivial || !only) continue;

        for(auto userPointer: ((Value*)phi)->uses(opt.local)) wake(U32(userPointer));
        for(auto input: phi->inputs.contents(opt.local)) wake(U32(input.value));

        opt.ir().replaceValue((ModulePtr<Value>)pointer, only);
        opt.ir().erasePhi(pointer);
        finish();
    }
}

}

void promotePlaces(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    auto& contained = containmentOf(opt);
    auto& reachable = reachableOf(opt);

    CandidateIndex byLocal;

    Array<Candidate> found;
    collectCandidates(opt, contained, found, byLocal);
    if(found.isEmpty()) return;

    /*
     * `available` is over the numbering the candidates had while the survey ran, and stays that way:
     * dropping one does not disturb the columns of the others, so each survivor carries the column
     * it had rather than the table being rebuilt around it - see `Candidate::column`.
     */
    IndexSetList available;
    IndexSet usable, readsAreWritten;
    surveyPlaces(opt, found, byLocal, reachable, available, usable, readsAreWritten);

    Array<Candidate> candidates;
    for(Size i = 0; i < found.size(); i++) {
        if(!usable[i]) continue;

        // A place read before anything wrote it is declined unless nothing writes it at all, which
        // is a shape with an answer of its own - see `fillUnwritten`.
        if(!readsAreWritten[i] && !fillUnwritten(opt, found[i])) continue;

        found[i].column = U32(i);
        candidates.push(::move(found[i]));
    }

    if(candidates.isEmpty()) return;

    // Rebuilt over the survivors, because the rewrite below looks a place up in it and the numbering
    // it was filled in with was the one the survey ran over. `column` is what still carries the old
    // one, and it is the only thing that reads the table.
    byLocal.build(candidates);

    auto count = opt.function->blocks.size();
    for(auto& candidate: candidates) {
        for(Size i = 0; i < count; i++) {
            candidate.entry.push(nullptr);
            candidate.exit.push(nullptr);
        }
    }

    /*
     * A phi only where the predecessors disagree, which is what a phi is for.
     *
     * The block list is in reverse postorder - the invariant `reorderBlocksInRpo` restores and
     * `lowerProgram` reads - so by the time a block is reached its predecessors have already said
     * what they leave the place holding, and the usual answer to "what does this block carry in" is
     * simply that value. Only a genuine disagreement between two predecessors, and a predecessor
     * that has not been visited, needs an instruction to merge them.
     *
     * Nothing here *depends* on the order being reverse postorder: a predecessor that has not been
     * settled yet is one this declines to read, so a list in some other order costs phis rather than
     * correctness. Forwarding a settled one is sound because it dominates - every path to this block
     * runs through one of the predecessors, each of those is dominated by the value's definition, so
     * the block is too.
     *
     * Built detached. `IrEditor::append` is what records a phi's alternatives as uses, so it may not
     * happen until they exist, and they are what the blocks below turn out to hold.
     */
    ScratchSet settled(opt.sets, count);

    Array<PlacedPhi> placed;

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        // Reachable blocks only. A load in code nothing runs has no value to be given - the analysis
        // above deliberately said nothing about such a block - and leaving it alone leaves the local
        // it reads alive, which is the right outcome for storage only dead code names.
        if(!reachable[block->index]) continue;

        for(Size i = 0; i < candidates.size(); i++) {
            auto& candidate = candidates[i];

            // A place nothing writes needs no phi anywhere: every block holds the same constant and
            // there is nothing for a join to choose between.
            if(candidate.initial) {
                candidate.entry[block->index] = candidate.initial;
                continue;
            }

            if(!available[block->index][candidate.column]) continue;

            ModulePtr<Value> only = nullptr;
            auto agreed = true;

            for(auto predecessor: block->incoming(opt.local)) {
                auto pred = opt.local[predecessor];
                if(!(*settled)[pred->index]) { agreed = false; break; }

                auto value = candidate.exit[pred->index];
                if(!value) { agreed = false; break; }

                if(!only) only = value;
                else if(only != value) { agreed = false; break; }
            }

            // Availability is the statement that every predecessor left something here, so the null
            // above is unreachable; it is tested rather than asserted because what a wrong answer
            // would cost is a phi rather than a value nothing wrote.
            if(agreed && only) {
                candidate.entry[block->index] = only;
                continue;
            }

            auto phi = createInst<InstPhi>(*opt.module, *opt.function, *block, block->source, StringId(),
                                           candidate.type);

            candidate.entry[block->index] = (ModulePtr<Value>)(phi - opt.local);
            placed.push(PlacedPhi { (ModulePtr<InstPhi>)(phi - opt.local), blockPointer, U32(i) });
        }

        rewriteBlock(opt, *block, candidates, byLocal);
        settled->set(block->index, true);
    }

    // The alternatives, now that every block has said what it leaves the place holding. One per
    // incoming edge and in that order, because that is the order a phi's inputs are matched against
    // predecessors in - resolve/lower.cpp maps the block, and codegen/js/flow.cpp emits the copy on
    // the edge itself.
    for(auto& entry: placed) {
        auto phi = (InstPhi*)opt.local[entry.phi];
        auto block = opt.local[entry.block];
        auto& candidate = candidates[entry.candidate];

        for(auto predecessor: block->incoming(opt.local)) {
            auto value = candidate.exit[opt.local[predecessor]->index];

            // Availability is exactly the statement that this is not null: every predecessor
            // either wrote the place itself or was carrying it in through a phi of its own.
            assertTrue(value != nullptr);
            phi->inputs.push(opt.program.arena, PhiInput { predecessor, value });
        }

        opt.ir().append(*block, phi);
    }

    removeTrivialPhis(opt, placed);
}
