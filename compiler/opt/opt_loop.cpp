#include "opt_pass.h"

/*
 * The two things worth doing to a loop here: moving what does not change out of one, and removing
 * one that does nothing at all.
 *
 * ---
 *
 * Loop-invariant code motion: a computation whose answer cannot change between iterations is moved
 * to the block in front of the loop, where it happens once.
 *
 * Two kinds of instruction move and the second is the one worth having:
 *
 *  - a *pure* value all of whose operands are defined outside the loop. Free of any question about
 *    order or effect, and mostly what the passes in front of this one leave behind: a packed field's
 *    mask, a `TypeMetric`, a `Symbol`, the arithmetic of an index that does not depend on the
 *    induction variable;
 *  - a *read* of a place nothing in the loop writes. This is the one the source actually contains -
 *    a bound compared against on every iteration, a field of a record the body only reads - and it
 *    is the reason this pass needs an aliasing answer rather than only a dominance one.
 *
 * ## Why a read may be hoisted at all
 *
 * Moving a load ahead of the loop makes it happen on a path where it might not have: a loop whose
 * test fails immediately never ran its body. That is only sound where the read cannot fail, which is
 * the same question `isDeadRead` in opt_value.cpp answers for removing one, and it gets the same
 * answer - a local or a global is storage the checker proved is there, while a pointer or a borrow
 * root is an address the program computed and the fault it may take is the program's to take.
 *
 * The preheader requirement carries the other half. `computeLoops` only names a block that leads
 * *only* into the header, so an instruction put there runs exactly when the loop is entered rather
 * than on some path that skips it.
 *
 * ## What is deliberately not moved
 *
 * Nothing with an effect and none of the ownership instructions - `Init`, `Assign`, `Move`, `Copy`,
 * `Drop`, `Swap`, `Exchange`, `Borrow`. A `Borrow` is the tempting one and it is exactly the one to
 * leave alone: hoisting a loan out of a loop extends it over iterations the borrow checker never
 * agreed to, which is a decision this stage has no standing to take. See opt.h.
 *
 * A call is not moved either, however invariant its arguments look. That needs an effect summary of
 * the callee, and `computeEffects` in resolve is the thing to reach for when it does - inventing a
 * second notion of purity here is risk 3 in Analysis-Optimization.md §7.
 */

namespace {

// Whether the block at this index is one the loop contains. Out of range is outside: `contains` is
// sized when the loop is found, and a block appended afterwards is one no loop knows about.
static bool insideLoop(Loop& loop, U32 index) {
    return index < loop.contains.size() && loop.contains[index];
}

/*
 * Whether a value is computed inside the loop.
 *
 * A constant is not, whatever block it names. One belongs to no block's instruction list - see
 * `eachFunctionValue` in opt.cpp - and `makeConstant` gives it the block of whatever it was built
 * for, so a folded constant inside a loop would otherwise look like a definition there and pin
 * everything that reads it. Asked through `isConstant` rather than by listing the four kinds, which
 * is the same list inst.def already holds.
 *
 * An argument is not either, for the same reason with a different cause: its block is the entry,
 * which no loop contains.
 *
 * Both passes in this file ask it, and of the same value graph - the hoister to decide what may
 * move out and the killer to decide what a dead loop still owes. Two spellings of it is one of them
 * learning about a new kind of definition and the other not.
 */
static bool definedInLoop(OptContext& opt, Loop& loop, ModulePtr<Value> value) {
    if(!value) return false;

    auto& definition = *opt.local[value];
    if(isConstant(definition) || definition.kind == Value::Arg) return false;
    if(!definition.block) return false;

    return insideLoop(loop, opt.local[definition.block]->index);
}

struct Hoister {
    OptContext& opt;
    Dominance& dominance;
    const IndexSet& contained;

    // Whether anything in this loop writes storage a hoisted read could see, decided once per loop
    // rather than per candidate - see `scanEffects`.
    bool anyClobber = false;
    Array<Place> written;

    bool definedIn(Loop& loop, ModulePtr<Value> value) { return definedInLoop(opt, loop, value); }

    bool operandsOutside(Loop& loop, Value& instruction) {
        auto outside = true;
        eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
            if(definedIn(loop, operand)) outside = false;
        });

        return outside;
    }

    /*
     * What the loop does to storage, in one walk.
     *
     * `written` collects every place the loop writes through an `Init` or an `Assign`, and
     * `anyClobber` records whether anything else that may write storage ran at all. The two are
     * separate because they are declined differently: a write is compared against a candidate and
     * may miss it, while a call is only survivable by a place a callee cannot reach.
     *
     * A place slot on a clobbering instruction is collected as a write as well. A `Move` out of a
     * contained local is the case: `anyClobber` does not save the candidate from it, because the
     * local being contained is exactly what makes `anyClobber` not apply.
     */
    void scanEffects(Loop& loop) {
        anyClobber = false;
        written.clear();

        for(auto index: loop.blocks) {
            auto block = opt.local[dominance.blocks[index]];

            for(auto pointer: block->instructions(opt.local)) {
                auto& instruction = *opt.local[pointer];

                switch(instruction.kind) {
                    case Value::Init:
                    case Value::Assign:
                        written.push(((InstInit&)instruction).place);
                        continue;

                    // Reads and fresh storage, exactly as opt_place.cpp's `clobbers` classifies
                    // them. An immutable borrow cannot be written through; a mutable one can, and
                    // whatever it was taken of is not a contained local anyway.
                    case Value::Alloc:
                    case Value::LoadPlace:
                    case Value::Copy:
                        continue;
                    default:
                        break;
                }

                if(isPureValue(instruction)) continue;

                anyClobber = true;
                eachPlace(instruction, [&](const Place& place) { written.push(place); });

                // And the storage a call is handed, for the reason opt_place.cpp forgets it at the
                // same instruction: `computeContainment` admits an unretained argument, so
                // `anyClobber` no longer saves a candidate from the one call that actually received
                // it. A hoisted read would then answer the first iteration's bytes forever.
                eachHandedLocal(opt, instruction, [&](U32 local) {
                    written.push(Place::inLocal(local));
                });
            }
        }
    }

    /*
     * Whether two places may reach the same storage.
     *
     * The same structural answer opt_place.cpp gives, and deliberately the weaker half of it: this
     * only has to say *no* correctly, and every case it is unsure about it answers yes. Two
     * different fields of one aggregate are the separation that matters, since that is what lets a
     * loop writing `p.x` keep a hoisted read of `p.y`.
     *
     * `framed` is what the caller has already proved about `b` - that it is rooted in a local this
     * frame contains - and it is the one thing that lets a pointer or a borrow be answered `no`.
     * `computeContainment` refuses a local any `Borrow` or `Address` names, so a contained local is
     * one no pointer in the function can be pointing at; opt_promote.cpp's `surveyCandidate` states
     * the same fact and relies on it, and this was the one walk in the directory that did not.
     *
     * It is what a loop writing through a subscript costs without it. `out[c] = row * n + c` writes
     * a borrow-rooted place, so every read of every counter in the loop aliased it and `row * n`
     * stayed in the body - while the identical loop that only reads hoisted it into the preheader.
     */
    bool mayAlias(const Place& a, const Place& b, bool framed) {
        if(a.root == PlaceRoot::Pointer || b.root == PlaceRoot::Pointer) return !framed;
        if(a.root == PlaceRoot::Borrow || b.root == PlaceRoot::Borrow) return !framed;

        if(a.root != b.root) return false;
        if(a.root == PlaceRoot::Local && a.local != b.local) return false;
        if(a.root == PlaceRoot::Global && a.global != b.global) return false;

        auto& leftPath = const_cast<Place&>(a).projections;
        auto& rightPath = const_cast<Place&>(b).projections;

        auto count = min(leftPath.size(), rightPath.size());
        for(Size i = 0; i < count; i++) {
            auto left = leftPath.get(opt.local, i);
            auto right = rightPath.get(opt.local, i);

            if(left.kind != right.kind) return true;

            switch(left.kind) {
                case ProjectionKind::Field:
                case ProjectionKind::Property:
                    if(left.index != right.index) return false;
                    break;
                case ProjectionKind::Downcast:
                    if(left.index != right.index) return true;
                    break;
                case ProjectionKind::Index: {
                    auto leftIndex = constantValueOf(opt, left.value);
                    auto rightIndex = constantValueOf(opt, right.value);
                    if(!leftIndex || !rightIndex) return true;
                    if(leftIndex.unwrap() != rightIndex.unwrap()) return false;
                    break;
                }
                case ProjectionKind::Deref:
                    return true;
                default:
                    break;
            }
        }

        return true;
    }

    /*
     * A read the loop cannot change the answer to.
     *
     * Four conditions, and each rules out one way of being wrong: the storage has to be one the read
     * cannot fault on, the path has to be one the loop does not compute (an index that moves is a
     * different element each time), nothing in the loop may write it, and where the loop calls
     * anything at all the storage has to be a local no callee can name.
     */
    bool invariantRead(Loop& loop, InstLoadPlace& load) {
        if(load.place.root != PlaceRoot::Local && load.place.root != PlaceRoot::Global) return false;

        // The value has to *be* the answer rather than name storage holding it, which is the same
        // test forwarding applies before it replaces a load with a value.
        if(!load.type || isUnit(opt.global, load.type) || isMemoryType(opt.global, load.type)) return false;

        for(Size i = 0; i < load.place.projections.size(); i++) {
            auto projection = load.place.projections.get(opt.local, i);
            if(definedIn(loop, projection.value)) return false;
        }

        // Asked once and used twice: it is what makes a call survivable, and it is also the proof
        // that no pointer or borrow the loop writes through can be naming this storage.
        auto framed = staysInFrame(opt, contained, load.place);
        if(anyClobber && !framed) return false;

        for(auto& place: written) {
            if(mayAlias(place, load.place, framed)) return false;
        }

        return true;
    }

    // Taking one instruction out of its block and putting it at the end of the preheader's, which is
    // in front of that block's terminator because a terminator is not in the list.
    void moveToPreheader(Loop& loop, ModulePtr<Inst> pointer) {
        opt.ir().moveInstruction(pointer, *opt.local[dominance.blocks[loop.preheader]]);
    }

    /*
     * One loop, in dominator preorder.
     *
     * The order is what makes a single walk enough: two candidates with a dependency between them
     * are in dominance order already - a use that is not a phi is dominated by its definition - so
     * visiting blocks in that order appends the definition to the preheader before the use, and a
     * value that became invariant *because* its operand was just hoisted is hoisted in the same
     * walk rather than on the next round.
     */
    void run(Loop& loop) {
        if(loop.preheader == Loop::kNone) return;

        scanEffects(loop);

        // Inline: this is parallel to the blocks of one loop, and is dropped before run() returns.
        SmallArray<U32, 32> order;
        for(auto index: loop.blocks) order.push(index);

        for(Size i = 1; i < order.size(); i++) {
            for(Size j = i; j > 0 && dominance.preorder[order[j]] < dominance.preorder[order[j - 1]]; j--) {
                swap(order[j], order[j - 1]);
            }
        }

        for(auto index: order) {
            auto block = opt.local[dominance.blocks[index]];

            for(Size i = 0; i < block->instructionCount(); i++) {
                auto pointer = block->instructionAt(opt.local, i);
                auto& instruction = *opt.local[pointer];

                auto movable = isPureValue(instruction)
                    ? operandsOutside(loop, instruction)
                    : instruction.kind == Value::LoadPlace &&
                      operandsOutside(loop, instruction) &&
                      invariantRead(loop, (InstLoadPlace&)instruction);

                if(!movable) continue;

                /*
                 * The read is leaving the loop, so it stops being one of the loop's own accesses -
                 * which matters where two reads of one place are both candidates and the first has
                 * already gone. Nothing has to be added to `written`: a hoisted instruction writes
                 * nothing, which is the whole of what made it movable.
                 */
                moveToPreheader(loop, pointer);
                i--;
            }
        }
    }
};

}

void hoistLoopValues(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    auto& dominance = opt.dominance;
    computeDominance(opt, dominance);

    Array<Loop> loops;
    computeLoops(opt, dominance, loops);
    if(loops.isEmpty()) return;

    ScratchSet contained(opt.sets, 0);
    computeContainment(opt, *contained);

    Hoister hoister { opt, dominance, *contained };

    // Innermost first, which `computeLoops` sorted for. A value hoisted out of an inner loop lands
    // in a block the outer one contains, so the outer loop's own walk - or the next driver round,
    // where the two are not nested directly - carries it the rest of the way out.
    for(auto& loop: loops) hoister.run(loop);
}

/*
 * A loop that counts and does nothing else, removed.
 *
 * Implementation-Containers.md §13.2 is what this is for and it is worth stating as a cost rather
 * than as a shape. `Reclaim(Array(a))` is one traversal over the live elements handing each to the
 * compiler's per-member teardown, which is the whole of what a container author writes - and for an
 * element type with nothing to run, the per-element work reduces to a read nobody reads. What is
 * left is a loop that counts to the array's length, and it was being emitted at every last use of
 * every `Array(Int)` in every program. The traversal is the right thing to have written; paying for
 * it when the element type is `Int` is not.
 *
 * ## What has to be true, and which way of being wrong each rules out
 *
 * 1. **The loop is left by failing the header's test and by nothing else.** That is what makes "the
 *    test is false" a complete description of removing it, which is the whole of the rewrite below.
 * 2. **Nothing in it writes storage** except the one write that steps the counter, and nothing in it
 *    calls anything. Reads are allowed and are the point - the element read is what the body is made
 *    of - and so are the ownership instructions' absence: a `Drop`, a `Move` or an `Init` of anything
 *    else is a decision the analyses took, and a loop containing one is not a loop that does nothing.
 * 3. **No value it defines is read after it**, the counter included. Removing the loop leaves the
 *    counter holding what it started with rather than the bound, so a reader outside would see a
 *    different number - which is why the local's own use list is checked and not only the values'.
 * 4. **It terminates**, and provably rather than by assumption. The shape admitted is the canonical
 *    one: a counter stepping by exactly one against a bound the loop does not compute, under a
 *    strict `<`. Then the counter never passes the bound, so it never wraps, so the iteration count
 *    is the difference between the two and the loop ends whatever it started at. Nothing here rests
 *    on a forward-progress rule, which the language has not got and would not want here.
 *
 * ## The read this removes that `eliminateDeadValues` would not
 *
 * A read through a raw pointer stays there - see `isDeadRead`, which declines one because the fault
 * it may take is the program's to take. This removes it, because the question is not the same one:
 * what is being decided is whether a *loop* with no effect may go, and the loop is where the address
 * came from. `let ->doomed = *(items + i)` over a run the count says is live is exactly a read that
 * cannot fault and that nothing here could prove cannot fault, and refusing the whole rule on its
 * account would be refusing it in the one case it exists for. It is the only place in this directory
 * that removes such a read, and it removes it only along with everything that computed its address.
 *
 * ## Why it is a constant rather than a deletion
 *
 * Nothing below deletes a block. What it knows is that the test is false the first time it is asked,
 * so it writes that down - and the arm nothing then reaches, the phi left with one alternative, and
 * the header folded back into the block above it are all opt_branch.cpp's, which already does that
 * job for a `je` on a constant and does the phi bookkeeping that goes with it. Which is also why
 * this runs immediately in front of `foldBranches` rather than beside the hoister.
 */

namespace {

struct LoopKiller {
    OptContext& opt;
    Dominance& dominance;

    bool inside(Loop& loop, U32 index) { return insideLoop(loop, index); }

    bool definedInside(Loop& loop, ModulePtr<Value> value) { return definedInLoop(opt, loop, value); }

    bool usedOutside(Loop& loop, ModulePtr<Value> value) {
        auto reader = opt.local[value];

        for(Size i = 0; i < reader->useCount(); i++) {
            auto block = opt.local[reader->useAt(opt.local, i)]->block;
            if(!block || !inside(loop, opt.local[block]->index)) return true;
        }

        return false;
    }

    // A read of one particular place, performed inside the loop. `samePlace` rather than the aliasing
    // question, because what this is establishing is that two instructions name the same counter.
    bool readsPlace(Loop& loop, ModulePtr<Value> value, const Place& place) {
        if(!definedInside(loop, value)) return false;
        if(opt.local[value]->kind != Value::LoadPlace) return false;

        return samePlace(opt, ((InstLoadPlace&)*opt.local[value]).place, place);
    }

    /*
     * Whether the counter is the loop's alone.
     *
     * Every use of its storage is either inside the loop or a write, and the write is what put the
     * starting value there. That one stays where it is - the local is then written and never read,
     * which is the state opt_scalar.cpp removes it in - and it is the reason this asks about the
     * local rather than only about the values the loop defines.
     */
    bool counterIsPrivate(Loop& loop, const Place& place) {
        if(place.local >= opt.function->localCount()) return false;

        auto storage = opt.function->localAt(opt.local, place.local).value;
        if(!storage) return false;

        auto owner = opt.local[storage];
        for(Size i = 0; i < owner->useCount(); i++) {
            auto pointer = owner->useAt(opt.local, i);
            auto user = opt.local[pointer];

            if(user->block && inside(loop, opt.local[user->block]->index)) continue;
            if(user->kind != Value::Init && user->kind != Value::Assign) return false;

            // A write *of* the counter rather than one that merely mentions it, since both are one
            // entry in the same list.
            auto& write = (InstInit&)*user;
            if(write.place.root != PlaceRoot::Local || write.place.local != place.local) return false;
            if(write.place.projections.isNotEmpty()) return false;
        }

        return true;
    }

    // Condition 4: `i = i + 1` against `i < bound`, at a width this stage knows, with the bound
    // outside the loop. See the header for why that is a termination proof rather than a guess.
    bool stepsToTheBound(Loop& loop, InstInit& step, InstJe& branch) {
        auto& place = step.place;

        // A whole local. A place with a path in it is a field of something, and nothing here has an
        // answer about what else reaches that something.
        if(place.root != PlaceRoot::Local || place.projections.isNotEmpty()) return false;

        auto added = opt.local[step.value];
        if(added->kind != Value::Add) return false;

        auto& sum = (InstBinary&)*added;
        auto increment = constantValueOf(opt, sum.rhs);
        if(!increment || increment.unwrap() != 1) return false;
        if(!readsPlace(loop, sum.lhs, place)) return false;

        if(!branch.cond || opt.local[branch.cond]->kind != Value::Cmp) return false;

        auto& test = (InstCmp&)*opt.local[branch.cond];
        if(test.cmp != CompareOp::Lt) return false;
        if(!readsPlace(loop, test.lhs, place)) return false;
        if(definedInside(loop, test.rhs)) return false;

        /*
         * And one width for all three, which is what the proof is stated in. `foldableInt` is the
         * same admission this stage's arithmetic uses, so a refinement or a type it declines to
         * compute in is a type this declines to reason about the wrapping of.
         */
        auto type = opt.local[test.lhs]->type;
        if(!foldableInt(opt, type)) return false;
        if(added->type != type || opt.local[sum.lhs]->type != type) return false;

        return counterIsPrivate(loop, place);
    }

    bool isRemovable(Loop& loop) {
        auto header = opt.local[dominance.blocks[loop.header]];
        if(!header->terminator()) return false;

        auto terminator = opt.local[header->terminator()];
        if(terminator->kind != Value::Je) return false;

        // Condition 1. The test continues the loop when it holds, which is the arrangement the `<`
        // below is read against - the other spelling is an ordinary loop and simply not this rule's.
        auto& branch = (InstJe&)*terminator;
        if(!inside(loop, opt.local[branch.thenBlock]->index)) return false;
        if(inside(loop, opt.local[branch.elseBlock]->index)) return false;

        for(auto index: loop.blocks) {
            if(index == loop.header) continue;

            auto block = opt.local[dominance.blocks[index]];
            if(!block->terminator()) return false;
            if(opt.local[block->terminator()]->kind != Value::Jmp) return false;
            if(!inside(loop, opt.local[((InstJmp&)*opt.local[block->terminator()]).target]->index)) return false;
        }

        /*
         * Conditions 2 and 3, in one walk.
         *
         * Every block in the loop is now on one path from the header back to it - each has a single
         * successor and every one of them reaches the latch - so the counter's single write runs
         * exactly once per iteration, which is what the step argument above assumes.
         */
        ModulePtr<Inst> step = nullptr;

        for(auto index: loop.blocks) {
            auto block = opt.local[dominance.blocks[index]];

            for(Size i = 0; i < block->phiCount(); i++) {
                if(usedOutside(loop, (ModulePtr<Value>)block->phiAt(opt.local, i))) return false;
            }

            for(Size i = 0; i < block->instructionCount(); i++) {
                auto pointer = block->instructionAt(opt.local, i);
                auto& instruction = *opt.local[pointer];

                if(usedOutside(loop, (ModulePtr<Value>)pointer)) return false;
                if(isPureValue(instruction) || instruction.kind == Value::LoadPlace) continue;

                if(instruction.kind == Value::Assign && !step) {
                    step = pointer;
                    continue;
                }

                return false;
            }
        }

        if(!step) return false;
        return stepsToTheBound(loop, (InstInit&)*opt.local[step], branch);
    }

    void run(Loop& loop) {
        if(!isRemovable(loop)) return;

        auto header = opt.local[dominance.blocks[loop.header]];
        auto terminator = header->terminator();
        auto& branch = (InstJe&)*opt.local[terminator];

        auto never = makeConstant(opt, *opt.local[terminator], opt.program.scalar.bool_, 0);

        opt.ir().replaceOperand(terminator, branch.cond, never);

        opt.changed = true;
    }
};

}

void eliminateDeadLoops(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    auto& dominance = opt.dominance;
    computeDominance(opt, dominance);

    Array<Loop> loops;
    computeLoops(opt, dominance, loops);
    if(loops.isEmpty()) return;

    LoopKiller killer { opt, dominance };

    // Innermost first, for the reason the hoister wants it: an outer loop whose whole body was an
    // inner one becomes removable itself, on the round after the inner one has gone.
    for(auto& loop: loops) killer.run(loop);
}
