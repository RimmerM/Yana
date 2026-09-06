#include "pass.h"

/*
 * A loop that counts and does nothing else, decided false at its header.
 *
 * What it is for is a cost the library cannot avoid writing. `Reclaim(Array(a))` in
 * `prelude/Core/Array.yana` is one traversal over the live elements handing each to the compiler's
 * per-member teardown, which is the whole of what a container author writes - and for an element
 * type with nothing to run, the per-element work reduces to a read nobody reads. What is left is a
 * loop that counts to the array's length, emitted at every last use of every `[Int]` in every
 * program. The traversal is the right thing to have written; paying for it when the element type is
 * `Int` is not.
 *
 * ## What has to be true, and which way of being wrong each rules out
 *
 * 1. **The loop is left by failing the header's test and by nothing else.** Every other block of it
 *    ends in a `jump` to a block inside, so "the test is false" is a complete description of not
 *    running it - which is the whole of the rewrite below.
 * 2. **Nothing in it writes storage** except the one write that steps the counter, and nothing in it
 *    calls, drops, allocates, forgets or synchronizes. Reads are allowed and are the point: the
 *    element read is what the body is made of.
 * 3. **No value it defines is read after it**, the counter's storage included - so the local's own
 *    use list is checked and not only the values'. Removing the loop leaves the counter holding what
 *    it started with rather than the bound, and a reader outside would see a different number.
 * 4. **It terminates**, and provably rather than by assumption. The shape admitted is the canonical
 *    one: a counter stepping by exactly one against a bound the loop does not compute, under a
 *    strict `<`. Then the counter never passes the bound, so it never wraps, so the iteration count
 *    is the difference between the two and the loop ends whatever it started at. Nothing here rests
 *    on a forward-progress rule, which the language has not got and would not want here.
 *
 * ## Where this stage differs from the shipping stage's version, and it is two places
 *
 * **The counter and the bound both live in storage, and the termination proof has to say so.** Over
 * there `promotePlaces` has already turned both into values by the time the loop passes run, so the
 * bound is a value defined outside the loop and the test reads it directly. Here promotion is
 * `promoteStackSlots`' at the lower tier - `resolve_ssa/README.md` says why conversion leaves locals
 * as storage - so what the header actually holds is `load %counter`, `load %bound`, `lt`. The bound's
 * load is therefore *inside* the loop and a rule that required it outside would fire on nothing at
 * all. What makes it invariant instead is condition 2: the only write in the loop is the counter's,
 * and two addresses rooted in two different `Alloc`s of this body cannot be the same bytes - so the
 * bound is admitted as a whole load of an allocation that is not the counter's.
 *
 * **The width is not asked for, only the type.** `foldableInt` declines a target-answered width, and
 * the counter of the loop this exists for is a `Size`. That refusal is about *arithmetic*: how wide
 * a `Size` turns out to be is the representation layer's answer and `optimizeProgram` runs once per
 * program. The termination argument needs no number - a value below a bound, stepped by one, is
 * still below or equal to it, whatever the width - so what it asks is that the counter, the step and
 * the bound are one integer type and that the step wraps in two's complement, which every integer
 * type here does.
 *
 * ## The read this removes that `simplify` would not
 *
 * A `Load` is not `kSsaPure` and `simplify` therefore never takes one, because a load reads storage
 * another instruction may write. This removes it, because the question is not the same one: what is
 * being decided is whether a *loop* with no effect may go, and the loop is where the address came
 * from. `let ->doomed = *advance(items, i)` over a run the count says is live is exactly a read that
 * cannot fault and that nothing here could prove cannot fault, and refusing the rule on its account
 * would be refusing it in the one case it exists for. It is the only place in this directory that
 * removes such a read, and it removes it only along with everything that computed its address.
 *
 * ## The ownership rule, and why it is one line
 *
 * Every value the loop defines and every argument its blocks declare must be `Trivial` or `Address`.
 * A tracked value the loop defines is one whose consumption is inside it, and deleting the body
 * leaves the consumption to the verifier's complaint; a tracked block argument on the header is a
 * value the entry edge hands over and nothing then consumes. A `Guaranteed` one is declined with
 * them for `replaceAllUses`' reason: what a borrow is measured against is its loan, and a loan whose
 * borrows have gone is not a fact this pass has any standing to move.
 *
 * A value defined *outside* the loop and consumed inside it needs no rule, and that is worth stating
 * rather than leaving to be re-derived: the header's exit edge is a path that does not consume it,
 * so IR in which one exists is IR the verifier has already rejected.
 *
 * ## Why it is a constant rather than a deletion
 *
 * Nothing here deletes a block. What it knows is that the test is false the first time it is asked,
 * so it writes that down - and the arm nothing then reaches, the block arguments left with one
 * alternative and the header folded back into the block above it are all `cleanCfg`'s, which does
 * that job for a branch on a constant and does the edge bookkeeping that goes with it. Which is also
 * why this runs immediately in front of it, README.md's section B rows 4 and 5.
 *
 * The **region** rule is the one every rewrite in this directory owes and it is satisfied by not
 * moving an edge at all: the branch keeps both its targets and both their arguments, and the pass
 * that removes them is the one that owns the rule.
 */

namespace ssa {

namespace {

struct LoopKiller {
    OptContext& opt;

    bool inside(Loop& loop, U32 index) { return loop.contains[index]; }

    Block* blockAt(U32 index) { return opt.body->block(opt.base, BlockId(index)); }

    // Whether a value is computed inside the loop. A constant is not, and neither is a block
    // argument of a block outside it: both are asked through the defining position rather than
    // through the kind, which is the one question `Value` answers directly.
    bool definedInside(Loop& loop, Value& value) {
        if(value.definer) {
            auto block = opt.base[value.definer]->block;
            return block && inside(loop, opt.base[block]->index.index);
        }

        return value.block && inside(loop, opt.base[value.block]->index.index);
    }

    /*
     * Whether anything outside the loop reads this value.
     *
     * The header's own terminator is inside, so a value the header computes and hands to the exit
     * block over its edge reads as "used inside" - and that is the right answer, because the header
     * is the one block of the loop this rewrite keeps. Everything the body defines is gone with it,
     * and a use anywhere outside is what this refuses.
     */
    bool usedOutside(Loop& loop, Value& value) {
        for(auto userPointer: value.uses(opt.base)) {
            auto block = opt.base[userPointer]->block;
            if(!block || !inside(loop, opt.base[block]->index.index)) return true;
        }

        return false;
    }

    // What a value the loop defines may be - the ownership rule, see the file comment.
    bool discardableConvention(Convention convention) {
        return convention == Convention::Trivial || convention == Convention::Address;
    }

    /*
     * The literal one value is, read without a width.
     *
     * `constantValueOf` is the folder's reader and puts every answer through `foldableInt`, which
     * declines a target-answered width - so it says nothing about `const 1` at a `Size`, which is
     * the step of the loop this pass exists for. Nothing here needs a width: the only question asked
     * of the literal is whether it is exactly one, and that is the same number in every register it
     * could be held in.
     */
    Maybe<U64> literalOf(Use used) {
        if(!used.isSet()) return Nothing();

        auto value = opt.base[used.value()];
        if(!value->definer) return Nothing();

        auto inst = opt.base[value->definer];
        if(inst->kind != Inst::Const) return Nothing();

        auto& constant = ((InstConst&)*inst).constant;
        if(constant.kind != mir::Constant::Int) return Nothing();

        return Just(constant.asInt());
    }

    // The allocation an address names outright, rather than the one it is rooted in. A projection is
    // refused here and not narrowed: what the counter rule needs is a whole slot, and a field of one
    // is a place something else may also name.
    Value* allocationOf(Use address) {
        if(!address.isSet()) return nullptr;

        auto value = opt.base[address.value()];
        if(!value->definer || opt.base[value->definer]->kind != Inst::Alloc) return nullptr;

        return value;
    }

    // A whole load of one allocation, performed inside the loop - which is what both the counter's
    // reads and the bound's are.
    Value* loadedAllocation(Loop& loop, Use used) {
        if(!used.isSet()) return nullptr;

        auto value = opt.base[used.value()];
        if(!definedInside(loop, *value) || !value->definer) return nullptr;

        auto inst = opt.base[value->definer];
        if(inst->kind != Inst::Load) return nullptr;

        return allocationOf(((InstLoad&)*inst).address);
    }

    /*
     * Condition 4: `i = i + 1` against `i < bound`, at one integer type, with the bound a place the
     * loop does not write.
     *
     * `step` is the loop's single store and `branch` the header's terminator, both found by the walk
     * below - so what is left here is that the two are about the same allocation and that the third
     * operand cannot move.
     */
    bool stepsToTheBound(Loop& loop, InstStore& step, InstBranch& branch) {
        auto counter = allocationOf(step.address);
        if(!counter) return false;

        // The step: an addition of one to a read of the counter.
        if(!step.value.isSet()) return false;

        auto stored = opt.base[step.value.value()];
        if(!stored->definer) return false;

        auto sum = opt.base[stored->definer];
        if(sum->kind != Inst::Binary || ((InstBinary&)*sum).op != BinaryOp::Add) return false;
        if(loadedAllocation(loop, ((InstBinary&)*sum).lhs) != counter) return false;

        auto increment = literalOf(((InstBinary&)*sum).rhs);
        if(!increment || increment.unwrap() != 1) return false;

        // The test: a strict `<` of a read of the same counter against something else.
        if(!branch.condition.isSet()) return false;

        auto condition = opt.base[branch.condition.value()];
        if(!condition->definer) return false;

        auto test = opt.base[condition->definer];
        if(test->kind != Inst::Binary || ((InstBinary&)*test).op != BinaryOp::Lt) return false;
        if(loadedAllocation(loop, ((InstBinary&)*test).lhs) != counter) return false;

        /*
         * And the bound, which is either computed above the loop or read out of a different slot.
         *
         * The second is the ordinary case here and the file comment is the argument: condition 2 has
         * already established that the loop's only write is this counter's, so a whole load of an
         * allocation that is not the counter answers the same number on every iteration. A load of
         * anything else - a field, a pointer, an address with no allocation of this body under it -
         * is declined rather than reasoned about.
         */
        auto bound = opt.base[((InstBinary&)*test).rhs.value()];
        if(definedInside(loop, *bound)) {
            auto place = loadedAllocation(loop, ((InstBinary&)*test).rhs);
            if(!place || place == counter) return false;
        }

        /*
         * One integer type for the counter, the step and the bound - which is what the proof is
         * stated in, and all of it that is needed. A width is deliberately not asked for: see the
         * file comment, where the difference from `foldableInt` is written down.
         */
        auto type = stored->type();
        if(!type || opt.decl[type]->kind != mir::Type::Int) return false;
        if(opt.base[((InstBinary&)*test).lhs.value()]->type() != type) return false;
        if(bound->type() != type) return false;

        return counterIsPrivate(loop, *counter);
    }

    /*
     * Whether the counter's storage is the loop's alone.
     *
     * Every use of the allocation is either inside the loop or a store *through* it, and that store
     * is what put the starting value there. It stays where it is - the slot is then written and
     * never read, which is the state `removeDeadStorage` takes it in - and it is the reason this
     * asks about the allocation rather than only about the values the loop defines.
     *
     * A use of the allocation as a *value* is refused at the same comparison, which is the case an
     * address handed to a callee arrives in: the storage would then be one something outside can
     * read whatever this walk saw.
     */
    bool counterIsPrivate(Loop& loop, Value& counter) {
        for(auto userPointer: counter.uses(opt.base)) {
            auto& user = *opt.base[userPointer];
            if(user.block && inside(loop, opt.base[user.block]->index.index)) continue;

            if(user.kind != Inst::Store) return false;

            auto& store = (InstStore&)user;
            if(!store.address.isSet() || opt.base[store.address.value()] != &counter) return false;
            if(store.value.isSet() && opt.base[store.value.value()] == &counter) return false;
        }

        return true;
    }

    /*
     * Conditions 1 to 3, in one walk, plus the single store condition 4 is about.
     *
     * The header is the block the test is in and every other block of the loop is on one path from
     * it back to it, so the counter's single write runs exactly once per iteration - which is what
     * the step argument above assumes and what the terminator rule below establishes.
     */
    Inst* removableStep(Loop& loop) {
        auto header = blockAt(loop.header);

        auto terminator = header->terminator(opt.base);
        if(!terminator || terminator->kind != Inst::Branch) return nullptr;

        /*
         * Condition 1. The test continues the loop when it holds, which is the arrangement the `<`
         * above is read against - the other spelling steps away from its bound and is simply not
         * this rule's.
         */
        auto& branch = (InstBranch&)*terminator;
        if(!branch.whenTrue.target || !branch.whenFalse.target) return nullptr;
        if(!inside(loop, opt.base[branch.whenTrue.target]->index.index)) return nullptr;
        if(inside(loop, opt.base[branch.whenFalse.target]->index.index)) return nullptr;

        Inst* step = nullptr;
        auto ok = true;

        loop.contains.forEach([&](Size index) {
            if(!ok) return;

            auto block = blockAt(U32(index));

            // Condition 1 again, for every block that is not the header: one plain jump, into the
            // loop. A block that branches, returns or ends nowhere is a second way out.
            if(U32(index) != loop.header) {
                auto last = block->terminator(opt.base);
                if(!last || last->kind != Inst::Jump) { ok = false; return; }

                auto& jump = (InstJump&)*last;
                if(!jump.to.target) { ok = false; return; }
                if(!inside(loop, opt.base[jump.to.target]->index.index)) { ok = false; return; }
            }

            for(auto argumentPointer: block->arguments(opt.base)) {
                auto argument = opt.base[argumentPointer];
                if(!discardableConvention(argument->convention)) { ok = false; return; }
                if(usedOutside(loop, *argument)) { ok = false; return; }
            }

            for(auto instPointer: block->instructions(opt.base)) {
                auto& inst = *opt.base[instPointer];

                for(auto resultPointer: inst.results.contents(opt.base)) {
                    auto result = opt.base[resultPointer];
                    if(!discardableConvention(result->convention)) { ok = false; return; }
                    if(usedOutside(loop, *result)) { ok = false; return; }
                }

                // Condition 2. Pure is the blanket answer - it is the column that says an
                // instruction may not be computed at all - and the two exceptions are the read the
                // body is made of and the one write that steps the counter.
                if(isPure(inst.kind) || isTerminator(inst.kind)) continue;
                if(inst.kind == Inst::Load) continue;

                if(inst.kind == Inst::Store && !step) {
                    step = &inst;
                    continue;
                }

                ok = false;
            }
        });

        if(!ok || !step) return nullptr;
        if(!stepsToTheBound(loop, (InstStore&)*step, branch)) return nullptr;

        return terminator;
    }

    bool run(Loop& loop) {
        auto terminator = removableStep(loop);
        if(!terminator) return false;

        auto& branch = (InstBranch&)*terminator;
        auto exit = opt.base[branch.whenFalse.target];
        auto source = terminator->source;

        /*
         * The test written down as the answer it has the first time it is asked.
         *
         * A fresh branch rather than an edited operand, because an operand is not something this
         * tier edits: the terminator is replaced, and `replaceTerminator`'s multiset rule is what
         * keeps both edges and the arguments they already carried - the two targets are unchanged,
         * so both edges are present in the old terminator and in the new one and neither is rebuilt.
         * The argument list handed over is therefore only the shape `makeBranch` needs.
         */
        auto boolean = mir::booleanType(opt.program);
        if(!boolean) return false;

        auto never = makeConstant(opt, *terminator, boolean, 0);

        SmallArray<Use, 4> arguments;
        for(auto used: branch.whenTrue.arguments.contents(opt.base)) arguments.push(used);

        auto editor = opt.ir();
        auto body = opt.base[branch.whenTrue.target];

        editor.replaceTerminator(*blockAt(loop.header),
                                 editor.makeBranch(source, editor.use(*never, UseKind::Inspect),
                                                   *body, *exit, arguments.pointer(),
                                                   arguments.size()));

        return true;
    }
};

} // namespace

void deadLoops(OptContext& opt) {
    auto& loops = loopsOf(opt);
    if(!loops.size()) return;

    LoopKiller killer { opt };

    // Innermost first, which `Loops` sorted for: an outer loop whose whole body was an inner one
    // becomes removable itself, on the round after the inner one has gone.
    for(auto& loop: loops) killer.run(loop);
}

} // namespace ssa
