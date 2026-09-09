/*
 * The sequence itself. See lower_pipeline.h for why it is here rather than in either resolve stage.
 */

#include "lower_pipeline.h"
#include "lower_forward.h"
#include "lower_split.h"
#include "lower_promote.h"
#include "lower_tail.h"
#include "lower_store.h"
#include "lower_cse.h"
#include "lower_licm.h"
#include "lower_recover.h"
#include "lower_strength.h"
#include "lower_divide.h"
#include "lower_induction.h"
#include "lower_merge.h"
#include "lower_thread.h"
#include "lower_fold.h"

void optimizeLowerFunction(LowerBase base, LowerModule& module, LowerFunction& fun,
                           LowerPtr<LowerValue> returnPlace, bool optimize) {
    /*
     * The two a build with the optimizer off still owes its backend, and then nothing else.
     *
     * Neither is an optimization. `makeDivisionTotal` is the language's answer for a division that
     * would fault, argued below where it stands in the full run. `foldDecidedBranches` is the
     * *target's*: a library arm gated on a compile-time constant is written as a branch, and the arm
     * a narrower machine cannot encode sits behind it. `lib/Native/Memory.native.yana`'s
     * `blockShort` tests `vectorBytes > 32` and loads a `Vec(U8, 32)` under it - which the x64
     * backend refuses outright below v3 - and that file's own comment already says the fold is what
     * removes it. So this is the pass the whole corpus turned out to be relying on the moment the
     * rest of the run went behind the gate, and it is here on the same terms the division rewrite
     * is: without it the compiler does not produce a slower program, it produces no program at all.
     *
     * Only the branch fold, and not a constant fold in front of it: the comparison is already a
     * literal by the time it arrives, because the lower builders fold as they build. Measured by
     * running the corpus with each half alone - the constant fold alone leaves twenty-one fixtures
     * failing in the backend, and the branch fold alone leaves none.
     */
    if(!optimize) {
        foldDecidedBranches(base, module, fun);
        makeDivisionTotal(base, module, fun);
        return;
    }

    // Every value a place names got storage of its own, because a place is an address and that is
    // the only shape either translation has - so a record built for somewhere else was built in a
    // temporary and copied there. Which of those temporaries the value could have been built in
    // directly is a question about storage rather than about ownership, which is why it is asked
    // here rather than in a resolve-tier pass. See lower_forward.h.
    //
    // Handed the hidden result pointer, where this function has one, so that a copy into it from a
    // temporary a call filled can be forwarded - see lower_forward.h, which is the whole of why this
    // pass needs to be told which parameter that is.
    forwardCopyDestinations(base, fun, returnPlace);

    // Then the slots that are still whole aggregates, cut into the fields their own accesses name -
    // see lower_split.h. Behind the forwarding, which has already removed the copies with a
    // temporary on one end, and in front of the promotion, which is the thing that makes a split
    // worth anything: every cell this produces is a slot loaded and stored as one value.
    splitAggregateSlots(base, fun);

    // Then which of the slots that are left actually needed memory, which is a question about the
    // finished IR rather than about the source - see lower_promote.h, and isDirectType in
    // resolve/type.h for why it is not asked any earlier. Behind the forwarding, so that a copy of a
    // whole slot promotion would have had to reproduce as a load or a store is one the forwarding
    // has already removed.
    promoteStackSlots(base, fun);

    // And then the forwarding once more, because promotion is what makes the *destination* of a copy
    // readable. `Just(build(..))` puts the pointer the record is going into through a slot of its
    // own - stored and loaded back a line later - so the copy's destination arrives here as a load
    // and is a value only after the slot is gone. §27 of test/bench/findings.md is the shape; the
    // second run is a per-block scan over a function most of whose copies the first run already
    // took, and it is what the first run cannot be moved behind, since promotion has strictly less
    // to do for every copy that has already gone.
    forwardCopyDestinations(base, fun, returnPlace);

    // Then the calls this function makes to itself with nothing left to do afterwards, which become
    // a loop round its own body - see lower_tail.h. Behind the promotion, because what it threads
    // through the loop is the accumulator that pass has just turned into a value, and in front of
    // everything below so that what they are shown is an ordinary loop.
    eliminateTailRecursion(base, module, fun);

    // What promotion turned into arithmetic over literals - a local's whole initial value is
    // assembled out of a load of the storage it is about to stop being. See lower_fold.h.
    foldFunctionConstants(base, module, fun);

    // Then the operations that are still divisions and multiplications, but by a number now known -
    // which is a question that could only be asked once the fold above had answered *which* operands
    // are literals. See lower_strength.h, and note that it emits arithmetic of its own, so the fold
    // runs again behind it before the dead immediates are swept.
    strengthReduceFunction(base, module, fun);
    foldFunctionConstants(base, module, fun);

    /*
     * Then the divisions this machine would raise on, given the answers the language defines for
     * them - see lower_divide.h. Behind the strength reduction and its fold, so that a divisor still
     * standing here is a runtime value or the one literal worth guarding, and in front of everything
     * below because what it leaves is a division that cannot fault: that is the premise `mayFault`
     * in lower/lower_inst.h now rests on, and the reason a division may be hoisted at all.
     *
     * A correctness pass rather than an optimization, which is why it is one of the two the return
     * at the top of this function still runs: doc/spec/types.md makes integer division total -
     * `x / 0` is 0, `x % 0` is `x`, `minimum / -1` is `minimum` - and neither native machine
     * provides any of it. Without it a runtime zero divisor raises `#DE` and takes the process down
     * instead of answering, which is not a slower program but a different one.
     *
     * With a fold behind it for the reason the pair above has one: it emits comparisons and selects
     * against literals, and a guarded division by a literal zero is a constant.
     */
    makeDivisionTotal(base, module, fun);
    foldFunctionConstants(base, module, fun);

    // And the branches whose answer the edge into them already settled - see lower_thread.h. Behind
    // the promotion, which is what turns the nested `Outcome` a `return` inside a `for` is written
    // as into the phis this reads, and behind the folds, so that an alternative that is a literal is
    // written as one. In front of the analysis below, because it changes the block set and every
    // pass past that point indexes by block.
    //
    // With a fold behind it for the reason the strength reduction has one: a phi left with a single
    // alternative is that alternative, and the comparison the next level out branches on folds
    // against it.
    threadDecidedBranches(base, module, fun);
    foldFunctionConstants(base, module, fun);

    // And the counters a loop carries two of because two things wanted one - see lower_cse.h. In
    // front of the analysis below rather than beside the CSE, because what it leaves is one value
    // where there were two and everything computed from either of them is then a pair the CSE
    // collects. It removes phis and the steps they held alive, and no blocks, so the loop structure
    // built below is the same one either way.
    mergeCongruentPhis(base, module, fun);

    /*
     * And the five passes that read the loop structure and the dominator tree, which is one loop
     * structure and one dominator tree - see LoopAnalysis in lower.h, which is where the argument
     * that they may share them lives. Built here rather than in each of them because two of the
     * things between them are folds: nothing in the run changes the block set, apart from the one
     * place below that says it did.
     */
    LoopAnalysis analysis { base, fun };

    // The computation this translation wrote down twice - an address assembled once per read of a
    // field, a promoted local's arithmetic repeated in each block that indexes with it, the quotient
    // sequence a program asking for both `x / d` and `x % d` gets twice. See lower_cse.h. Behind the
    // strength reduction so that the last of those is one of the shapes it can see, and in front of
    // the loop pass so that what that pass is shown is one multiply rather than three.
    auto rewired = eliminateCommonValues(base, module, fun, analysis);

    // Then the load a guard's slow arm invalidated and its fast arm did not, which is what the pass
    // above has just retired for the second time - see lower_recover.h. Directly behind it, because
    // what is left in a join after it is either partially redundant or not redundant at all, and in
    // front of everything below because the value it recovers is one the loop passes then see as
    // loop-invariant rather than as a load.
    recoverPartialLoads(base, module, fun);

    // Then the loads that read back a word the store above them still has in a register, and the
    // stores nothing between the two can have observed - which is what writing two `@bits` fields of
    // one word is written as. See lower_store.h. Behind the CSE, and that is the whole of why it is
    // here rather than beside the promotion: the two accesses of one field are written as two
    // `add %self, 8`, so "the same pointer" is only true of them once those are one value. What
    // reads the value it forwards is the mask folding in the last fold below.
    forwardStoredValues(base, module, fun);

    // And the pair again where the CSE took a decided arm, which is the one thing in this run that
    // removes an edge and renumbers the blocks the two are indexed by. Conditional rather than
    // unconditional because most functions have no such branch in them, and the four passes below
    // are the ones paying for the answer being right.
    if(rewired) analysis.rebuild(base, fun);

    // And what a loop repeats because it was written inside it - the reads whose address does not
    // change while nothing in the loop writes, and the arithmetic whose operands do not change at
    // all. See lower_licm.h, and §10 item 1 and §50.2 of test/bench/findings.md for why the
    // resolve-tier hoister above cannot reach either. Behind the CSE so that a loop reading one
    // address twice is shown one load.
    hoistLoopInvariants(base, module, fun, analysis);

    // The multiply that is left because its other operand is the loop's own counter, which is not a
    // strength reduction of one operation into another but of a whole recurrence - see
    // lower_induction.h. After the fold above, since what it needs to read is the stride as a
    // number; it emits an immediate of its own, so the fold and the sweep run behind it.
    reduceInductionVariables(base, module, fun, analysis);
    foldFunctionConstants(base, module, fun);

    // And the sign extension that is left because the counter driving it is narrower than the
    // address it feeds, which the pass above only removes where it replaced the address outright -
    // see lower_induction.h. Behind it for exactly that reason, and in front of one more fold
    // because the widened start of a counter that began at zero is a cast of a literal.
    widenInductionVariables(base, module, fun, analysis);
    foldFunctionConstants(base, module, fun);

    // And the bounds check a counted loop's own test has already made - see lower_induction.h, and
    // §28 of test/bench/findings.md. Behind the widening, because after it the counter is already
    // the width the check compares at and the two tests name one value. Last of the three, because
    // dropping the abort blocks it strands is the first thing here that changes the block set, and
    // so is what ends the analysis above.
    eliminateBoundedChecks(base, module, fun, analysis);

    // And the branches one of the passes since `threadDecidedBranches` decided - a phi the CSE
    // unified, a counter the widening made a literal, a truth value the fold reduced to the number
    // it always was. See lower_thread.h. Here rather than up there because that is where the
    // deciding happened, and here rather than one line higher because this changes the block set:
    // the loop analysis above has already been ended by the check elimination.
    foldDecidedBranches(base, module, fun);

    // And the exits inlining brought several copies of, along with the teardown every early return
    // of one function writes out again - see lower_merge.h, and §32 of test/bench/findings.md. Last,
    // because two copies of one arm only agree once everything above has folded them the same way,
    // and because what it removes are whole blocks rather than instructions: nothing here gains from
    // being shown one exit instead of four, and every one of them gains from having run.
    //
    // In front of the sweep rather than behind it, since a block that goes takes the last reader of
    // whatever it read with it - the `0` three copies of `ret 0` shared is dead once one of them is
    // left.
    mergeDuplicatedExits(base, module, fun);
    removeDeadValues(base, module.arena, fun);
}
