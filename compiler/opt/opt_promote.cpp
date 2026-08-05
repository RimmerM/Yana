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
 * writes of its own place has had its address handed to nothing, so no call, no borrow, no pointer
 * and no capture can reach it. Which means the *only* instructions that can write it are the ones
 * this pass can see, and the only places that can overlap one of its fields are other places rooted
 * in the same local. That is what removes the aliasing question entirely, and it is why this needs no
 * clobber rule and no notion of a barrier - a call in the middle of the function is not an event at
 * all here.
 *
 * The reasoning is opt_scalar.cpp's, one step further: that pass uses the use list to prove a local
 * is *written and never read*, and removes it. This one uses the same list to prove a local is
 * private, and removes the reads so that the other pass can then see it that way. Which is the point
 * of the pair - neither turns a constructed record back into values on its own.
 *
 * ## The algorithm
 *
 * lower_promote.cpp's, over places instead of allocas, and the same deliberate simplification: rather
 * than compute an iterated dominance frontier, place a phi in *every* block the place is known to
 * arrive already written, then delete the ones whose alternatives all agree. The two produce the same
 * IR, and "known to arrive written" is a plain forward AND-dataflow that has to be computed anyway -
 * it is what decides that every alternative of a phi exists, and that no read is of storage nothing
 * ever put anything in. It is solved from the *optimistic* end, which is the one place this departs
 * from that file and the reason it can do anything about a loop at all - see `computeAvailability`.
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

    IndexSet stores;    // whether the block writes the place at all
    IndexSet available; // whether every path into the block has written it
    ValueList entry;
    ValueList exit;
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

Size candidateOf(OptContext& opt, Array<Candidate>& candidates, const Place& place) {
    for(Size i = 0; i < candidates.size(); i++) {
        if(samePlace(opt, candidates[i].place, place)) return i;
    }

    return maxLimit<Size>;
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
void collectCandidates(OptContext& opt, const IndexSet& contained, Array<Candidate>& into) {
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        for(auto pointer: opt.local[blockPointer]->instructions(opt.local)) {
            auto instruction = opt.local[pointer];
            if(instruction->kind != Value::LoadPlace) continue;

            auto& load = (InstLoadPlace&)*instruction;
            if(load.place.projections.isEmpty()) continue;
            if(!holdsLoadableValue(opt, load.type)) continue;
            if(!staysInFrame(opt, contained, load.place)) continue;
            if(candidateOf(opt, into, load.place) != maxLimit<Size>) continue;

            Candidate candidate;
            candidate.place = load.place;
            candidate.type = load.type;
            into.push(::move(candidate));
        }
    }
}

/*
 * Which blocks write the place, and whether anything else in the function may reach it.
 *
 * "Anything else" is a much shorter list than it would be for arbitrary storage, and that is the
 * whole value of the containment proof: a place rooted in a pointer or a borrow cannot name a
 * contained local, a call cannot reach one, and a place rooted in a *different* local is different
 * storage by construction. So the only thing that can disturb this place is another access rooted in
 * the same local, and the only one that disturbs it is a write to an overlapping path.
 *
 * The type has to be the same at every access for the same reason a phi's alternatives do: what
 * replaces the reads is one value, and one value has one type. Reading a place at two types is not
 * something the resolver emits - a projection has the field's type - but a `Unit` path introduced by
 * the packing expansion is a reading of storage rather than of a field, and this is what would
 * notice if two of those ever disagreed.
 */
bool surveyCandidate(OptContext& opt, Candidate& candidate) {
    candidate.stores.reset(opt.function->blocks.size());

    auto usable = true;

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(auto pointer: block->instructions(opt.local)) {
            auto instruction = opt.local[pointer];
            auto written = isWrite(*instruction) || instruction->kind == Value::Aggregate;

            auto visit = [&](const Place& place, ModulePtr<Value> stored) {
                if(place.root != PlaceRoot::Local) return;
                if(place.local != candidate.place.local) return;

                if(samePlace(opt, place, candidate.place)) {
                    if(!written) {
                        // One value replaces every read, and one value has one type - so a read of
                        // this storage at another type is not something to answer, whatever produced
                        // it. See `collectCandidates`, which took the type from the first one.
                        if(instruction->kind == Value::LoadPlace &&
                           instruction->type != candidate.type) usable = false;

                        return;
                    }

                    if(opt.local[stored]->type != candidate.type) usable = false;

                    candidate.stores.set(block->index, true);
                    return;
                }

                if(!pathsMayOverlap(opt, place, candidate.place)) return;
                if(!isRead(*instruction)) { usable = false; return; }

                /*
                 * A read of an aggregate is the one read that may not be one.
                 *
                 * `LoadPlace` of a memory type does not answer with the storage's contents - there is
                 * no value of that shape to answer with - so what its result stands for is the
                 * storage itself, and anything that consumes one may be writing through it. The
                 * resolver does not build that shape today: an aggregate handed to a call or written
                 * whole is the `Alloc` itself as an operand, which containment already declines, and
                 * the whole-record load in front of every field access is read by nothing and removed
                 * by `isDeadRead`. So this costs nothing now and is what would notice if that ever
                 * stopped being true.
                 */
                if(instruction->kind != Value::LoadPlace) return;
                if(!holdsLoadableValue(opt, instruction->type) && instruction->useCount() != 0) {
                    usable = false;
                }
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

            if(!usable) return false;
        }
    }

    return true;
}

/*
 * Where the place is known to hold something.
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
 */
void computeAvailability(OptContext& opt, Candidate& candidate, const IndexSet& reachable) {
    auto count = opt.function->blocks.size();

    candidate.available.reset(count);

    ScratchSet out(opt.sets, count);
    out->copyFrom(reachable);

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];
            if(!reachable[block->index]) continue;

            auto incoming = block->incoming(opt.local);

            auto in = incoming.size() != 0;
            for(auto predecessor: incoming) {
                if(!(*out)[opt.local[predecessor]->index]) { in = false; break; }
            }

            if(candidate.available[block->index] != in) {
                candidate.available.set(block->index, in);
                changed = true;
            }

            auto leaves = in || candidate.stores[block->index];
            if((*out)[block->index] != leaves) {
                out->set(block->index, leaves);
                changed = true;
            }
        }
    }
}

// Whether every read of the place follows something that wrote it. The one thing that can still
// disqualify a place at this point, and it disqualifies it completely: a value cannot carry what was
// never put in it.
bool readsAreWritten(OptContext& opt, Candidate& candidate, const IndexSet& reachable) {
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];
        if(!reachable[block->index]) continue;

        U8 written = candidate.available[block->index];

        for(auto pointer: block->instructions(opt.local)) {
            auto instruction = opt.local[pointer];

            if(instruction->kind == Value::Aggregate) {
                eachWrittenPlace(opt, *instruction, [&](const Place& place, ModulePtr<Value>) {
                    if(samePlace(opt, place, candidate.place)) written = 1;
                });
            } else {
                eachPlace(*instruction, [&](const Place& place) {
                    if(!samePlace(opt, place, candidate.place)) return;

                    if(isWrite(*instruction)) written = 1;
                    else if(!written) written = 2;
                });
            }

            if(written == 2) return false;
        }
    }

    return true;
}

/*
 * The rewrite: one pass over each block, answering every read from what the place is holding.
 *
 * The writes stay where they are, so this walks the list rather than rebuilding it - the only
 * instruction it removes is a load whose readers have all been pointed elsewhere.
 */
void rewriteBlock(OptContext& opt, Block& block, Array<Candidate>& candidates) {
    ValueList current;
    for(auto& candidate: candidates) current.push(candidate.entry[block.index]);

    for(Size i = 0; i < block.instructionCount(); i++) {
        auto pointer = block.instructionAt(opt.local, i);
        auto instruction = opt.local[pointer];

        if(instruction->kind == Value::LoadPlace) {
            auto which = candidateOf(opt, candidates, ((InstLoadPlace&)*instruction).place);
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
                auto which = candidateOf(opt, candidates, place);
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
 * change to the function - this pass places one in every block the place reaches, so most of them are
 * that. A phi whose alternatives all agree *is* a change, because something reads it and now reads
 * the value directly instead; a self-reference does not count as an alternative, which is what
 * collapses a loop-carried phi whose body never writes the place.
 *
 * Iterated, because removing one can make the next one trivial - which is how a chain of maximal phis
 * down a straight-line region collapses to nothing.
 */
void removeTrivialPhis(OptContext& opt, Array<ModulePtr<InstPhi>>& phis) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(Size at = 0; at < phis.size(); at++) {
            auto pointer = phis[at];
            auto phi = opt.local[pointer];

            if(phi->useCount() == 0) {
                opt.ir().erasePhi(pointer);
                phis.remove(at--);
                changed = true;
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

            opt.ir().replaceValue((ModulePtr<Value>)pointer, only);
            opt.ir().erasePhi(pointer);
            phis.remove(at--);
            changed = true;
        }
    }
}

}

void promotePlaces(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    ScratchSet contained(opt.sets, 0);
    computeContainment(opt, *contained);

    ScratchSet reachable(opt.sets, 0);
    computeReachable(opt, *reachable);

    Array<Candidate> found;
    collectCandidates(opt, *contained, found);
    if(found.isEmpty()) return;

    Array<Candidate> candidates;
    for(auto& candidate: found) {
        if(!surveyCandidate(opt, candidate)) continue;

        computeAvailability(opt, candidate, *reachable);
        if(!readsAreWritten(opt, candidate, *reachable)) continue;

        candidates.push(::move(candidate));
    }

    if(candidates.isEmpty()) return;

    auto count = opt.function->blocks.size();
    for(auto& candidate: candidates) {
        for(Size i = 0; i < count; i++) {
            candidate.entry.push(nullptr);
            candidate.exit.push(nullptr);
        }
    }

    /*
     * A phi wherever the place arrives already written, which is every block a value has to be merged
     * into and a good many where it does not - see `removeTrivialPhis`.
     *
     * Built detached. `IrEditor::append` is what records a phi's alternatives as uses, so it may not
     * happen until they exist, and they are what the blocks below turn out to hold.
     */
    Array<ModulePtr<InstPhi>> phis;
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(auto& candidate: candidates) {
            if(!candidate.available[block->index]) continue;

            auto phi = createInst<InstPhi>(*opt.module, *opt.function, *block, block->source, 0,
                                           candidate.type);

            candidate.entry[block->index] = (ModulePtr<Value>)(phi - opt.local);
        }
    }

    // Reachable blocks only. A load in code nothing runs has no value to be given - the analysis
    // above deliberately said nothing about such a block - and leaving it alone leaves the local it
    // reads alive, which is the right outcome for storage only dead code names.
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];
        if((*reachable)[block->index]) rewriteBlock(opt, *block, candidates);
    }

    // The alternatives, now that every block has said what it leaves the place holding. One per
    // incoming edge and in that order, because that is the order a phi's inputs are matched against
    // predecessors in - resolve/lower.cpp maps the block, and codegen/js/flow.cpp emits the copy on
    // the edge itself.
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(auto& candidate: candidates) {
            if(!candidate.available[block->index]) continue;

            auto phi = (InstPhi*)opt.local[candidate.entry[block->index]];

            for(auto predecessor: block->incoming(opt.local)) {
                auto value = candidate.exit[opt.local[predecessor]->index];

                // Availability is exactly the statement that this is not null: every predecessor
                // either wrote the place itself or was carrying it in through a phi of its own.
                assertTrue(value != nullptr);
                phi->inputs.push(opt.program.arena, PhiInput { predecessor, value });
            }

            opt.ir().append(*block, phi);
            phis.push((ModulePtr<InstPhi>)(phi - opt.local));
        }
    }

    removeTrivialPhis(opt, phis);
}
