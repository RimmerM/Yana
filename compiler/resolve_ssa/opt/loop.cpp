#include "pass.h"

/*
 * A loop that counts and does nothing else, decided false at its header.
 *
 * What it is for is a cost the library cannot avoid writing. `Reclaim(Array(a))` in
 * `lib/Core/Array.yana` is one traversal over the live elements handing each to the compiler's
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
 * 2. **Nothing in it writes storage**, and nothing in it calls, drops, allocates, forgets or
 *    synchronizes: every instruction is `kSsaPure`, a terminator or a `Load`. Reads are allowed and
 *    are the point - the element read is what the body is made of. A `Store` is no longer the one
 *    exception it used to be, and the section below is why.
 * 3. **Nothing it produces is read after it** - no value it defines and no argument any of its
 *    blocks declares, the header's own included, because the header's arguments are what the counter
 *    now is. Removing the loop leaves such an argument answered by the entry edge, so it holds what
 *    the counter started at rather than what it counted to, and a reader outside would see a
 *    different number.
 * 4. **It terminates**, and provably rather than by assumption. The shape admitted is the canonical
 *    one: a counter stepping by exactly one against a bound the loop does not compute, under a
 *    strict `<`. Then the counter never passes the bound, so it never wraps, so the iteration count
 *    is the difference between the two and the loop ends whatever it started at. Nothing here rests
 *    on a forward-progress rule, which the language has not got and would not want here.
 * 5. **Nothing tracked is left behind.** Every value the loop defines and every argument its blocks
 *    declare is `Trivial` or `Address`.
 *
 * ## The proof is over values, and that is what phase 13 changed
 *
 * The pass was first written over storage, because at that point every local was one: the counter
 * was an `Alloc`, the step was `store %c, add(load %c, 1)` and the test was `lt(load %c, bound)`.
 * `promote` runs first in the round and takes exactly that slot, so the counter arrives here as a
 * **block argument of the header** whose operand on the back edge is the `add` and whose operand on
 * the entry edge is what it started at - and the same rule read literally fired on nothing at all.
 * Phase 11 measured it at zero and phase 13 restated it here.
 *
 * The storage form was **not** kept beside this one. Measured over the whole corpus with the value
 * form absent, it fires zero times out of 1055 loops offered: promotion is not a case that sometimes
 * applies, and a slot it declines - a borrowed parameter's, an escaping one - is a slot whose loads
 * this pass could not have called invariant either. Two statements of one termination proof, one of
 * which never answers, is the arrangement `sameExtra` exists to warn about one tier down.
 *
 * Three things follow from the counter being a value rather than a slot, and each is a condition
 * above read differently rather than a new one:
 *
 *  - **condition 2 has no exception left.** The step is an `add`, which is pure, so the one `Store`
 *    the storage form admitted has nothing to admit. A store still standing in the loop is a slot
 *    `promote` declined, and the paragraph above is the answer to it.
 *  - **condition 3 asks about block arguments and about the edge that leaves.** The counter's final
 *    value used to live in the slot and the allocation's use list was where a reader outside showed
 *    up; now it is a header argument, and a reader outside is either an instruction in a block
 *    outside or an operand handed over the header's exit edge. The second needs saying because the
 *    header's terminator is *inside*: a value it computes for the block after the loop reads as
 *    "used inside" through the block alone, and it is not. The exact rule would admit one whose
 *    receiving argument nothing reads; this declines every use outside, which is the simplest rule
 *    that is sound and which nothing in the corpus pays for.
 *  - **the bound is usually already outside.** `hoist` runs earlier in the same round and moves an
 *    invariant read into the preheader, so the ordinary case is now a bound defined above the loop -
 *    which is the rule the shipping stage's `eliminateDeadLoops` has, arrived at from the other end.
 *    A read left inside is still admitted, and what makes it invariant is `mayWriteStorage` asked of
 *    every instruction of the loop against the allocation the address is rooted in. Condition 2 has
 *    in fact already left nothing that could write - a `Store` is refused, a `Call`, an `Atomic` and
 *    a `Drop` are not pure, and a tracked `Load` result is refused by condition 5 - so that walk
 *    answers no every time it is asked. It is asked anyway, because the alternative is a proof that
 *    holds only for as long as condition 2 stays exactly this shape.
 *
 * **The width is not asked for, only the type.** `foldableInt` declines a target-answered width, and
 * the counter of the loop this exists for is a `Size`. That refusal is about *arithmetic*: how wide
 * a `Size` turns out to be is the representation layer's answer and `optimizeProgram` runs once per
 * program. The termination argument needs no number - a value below a bound, stepped by one, is
 * still below or equal to it, whatever the width - so what it asks is that the counter, the step and
 * the bound are one integer type. Both signednesses are admitted for the same reason: `i < bound`
 * implies `i + 1 <= bound` under either reading of `<`, and the literal one is read by `literalOf`
 * rather than by `constantValueOf`, which puts every answer through `foldableInt` and therefore says
 * nothing about `const 1` at a `Size`.
 *
 * **A step of exactly one, and not merely a positive one.** The no-wrap argument is what the width
 * would otherwise have to supply: `i < bound` gives `i <= bound - 1`, so a step of one lands at or
 * below the bound and cannot pass the top of the type, whatever the top is. A step of `k` lands at
 * `bound - 1 + k`, which for `k` above one is a value this pass would have to know the width to rule
 * out - and `bound` at the type's maximum with `k` of three is a loop that wraps past its own test
 * and runs forever. Admitting a larger step means asking `foldableInt` for a width and declining a
 * `Size`, which is declining the loop this pass exists for.
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
 * The header's own arguments go the same way and by the same route, which phase 13 checked rather
 * than assumed: once the body is unreachable the header has one incoming edge, and `mergeInto` is
 * what answers a block argument by the operand its one surviving edge hands it.
 *
 * What this pass *reads* is the other half of the same arrangement, and phase 14 is where it became
 * one: `removeTrivialArguments` takes out an argument every incoming edge answers alike, whoever
 * placed it, so a `for`'s bound and step are gone from the header before conditions 3 and 4 are
 * asked. Without it the desugaring's `bb1(%bound, %step, %counter)` declined this pass twice - the
 * bound left the loop over the exit edge, which condition 3 refuses, and the step arrived as a block
 * argument where `literalOf` wants a `Const`. Neither proof was relaxed to fix that, and neither
 * should be: what was wrong was the shape they were asked about.
 *
 * The **region** rule is the one every rewrite in this directory owes and it is satisfied by not
 * moving an edge at all: the branch keeps both its targets and both their arguments, and the pass
 * that removes them is the one that owns the rule.
 */

namespace ssa {

namespace {
struct LoopKiller {
    OptContext& opt;

    // Read once at the top and held, on `analysis.h`'s discipline. Sound here for the reason the
    // hoister's is: this pass adds no use of any allocation - it replaces one terminator with
    // another naming the same edge operands - so an answer taken before the rewrite still holds
    // after it.
    const Contained& contained;

    bool inside(Loop& loop, U32 index) { return loop.contains[index]; }

    Block* blockAt(U32 index) { return opt.body->block(opt.base, BlockId(index)); }

    Value* valueOf(Use used) { return used.isSet() ? opt.base[used.value()] : nullptr; }

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
     * Whether anything outside the loop reads this value - condition 3.
     *
     * Two ways of being read outside, and the second is the one the block alone does not answer. An
     * instruction in a block outside is the plain case. An operand carried over an edge **out** of
     * the loop is the other: the only such edge is the header's own exit arm, the header's
     * terminator is inside, and a value handed over it would therefore read as "used inside" through
     * the defining block of the user. It is not - the header keeps its arguments and the entry edge
     * answers them, so what arrives at the block after the loop is what the counter started at.
     */
    bool usedOutside(Loop& loop, Value& value) {
        for(auto userPointer: value.uses(opt.base)) {
            auto& user = *opt.base[userPointer];
            if(!user.block || !inside(loop, opt.base[user.block]->index.index)) return true;
            if(!isTerminator(user.kind)) continue;

            auto leaves = false;
            forEachEdge(opt.base, user, [&](Edge& edge) {
                if(!edge.target || inside(loop, opt.base[edge.target]->index.index)) return;

                for(auto used: edge.arguments.contents(opt.base)) {
                    if(used.isSet() && opt.base[used.value()] == &value) leaves = true;
                }
            });

            if(leaves) return true;
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

    // Where a value stands in a block's argument list, which is the position every edge into that
    // block fills - `~Size(0)` for a value that is not one of them.
    Size argumentIndex(Block& block, Value& value) {
        for(Size i = 0; i < block.argumentCount(); i++) {
            if(opt.base[block.argumentAt(opt.base, i)] == &value) return i;
        }

        return ~Size(0);
    }

    /*
     * Whether one operand handed back to the header is the counter plus one.
     *
     * Either way round, because addition commutes and nothing normalizes which side a constant sits
     * on: `simplify` folds an addition of two constants and leaves an addition of a value and one
     * alone. The result's type is compared rather than the operands', since that is the type the
     * argument is declared at and the one the proof is stated in.
     */
    bool stepsByOne(Value& counter, Use used, DeclPtr<mir::Type> type) {
        auto value = valueOf(used);
        if(!value || !value->definer || value->type() != type) return false;

        auto inst = opt.base[value->definer];
        if(inst->kind != Inst::Binary || ((InstBinary&)*inst).op != BinaryOp::Add) return false;

        auto& sum = (InstBinary&)*inst;

        Maybe<U64> step;
        if(valueOf(sum.lhs) == &counter) step = literalOf(sum.rhs);
        else if(valueOf(sum.rhs) == &counter) step = literalOf(sum.lhs);

        return step && step.unwrap() == 1;
    }

    // Whether anything in the loop may write the storage a read inside it answers from - the same
    // predicate `forwardMemory`, `cse` and `hoist` ask, so a call that was handed the address, an
    // atomic, a drop and a handover all end a candidate here as they do there.
    bool loopWrites(Loop& loop, Value& allocation) {
        auto written = false;

        loop.contains.forEach([&](Size index) {
            if(written) return;

            auto block = blockAt(U32(index));
            for(auto pointer: block->instructions(opt.base)) {
                if(mayWriteStorage(opt, contained, *opt.base[pointer], allocation)) written = true;
            }
        });

        return written;
    }

    /*
     * Whether the bound answers the same number on every iteration.
     *
     * Computed above the loop is the ordinary case since `hoist` arrived - it moves an invariant read
     * into the preheader one pass earlier in the same round. A read still inside is admitted through
     * the storage it answers from: an address the loop does not compute, rooted in an allocation of
     * this body, that nothing in the loop writes. A load through anything this body cannot root - a
     * global, a pointer out of memory, a borrowed parameter's pointee - is declined rather than
     * reasoned about, because `allocationRoot`'s null is the permissive answer and there is nothing
     * to ask `mayWriteStorage` about.
     */
    bool invariantBound(Loop& loop, Value& bound) {
        if(!definedInside(loop, bound)) return true;
        if(!bound.definer) return false;

        auto inst = opt.base[bound.definer];
        if(inst->kind != Inst::Load) return false;

        auto address = valueOf(((InstLoad&)*inst).address);
        if(!address || definedInside(loop, *address)) return false;

        auto root = allocationRoot(opt, address);
        return root && !loopWrites(loop, *root);
    }

    /*
     * Condition 4: `i < bound` at the header, with `i` an argument of the header that every edge
     * back into it hands `i + 1`.
     *
     * The counter is a block argument rather than a slot and that is phase 13's restatement - see
     * the file comment. What has to hold of it is that it never passes the bound: it enters below
     * one, every way round adds exactly one to it, and one integer type carries all three, so the
     * iteration count is the difference between where it started and where it stops.
     */
    bool countsToTheBound(Loop& loop, Block& header, InstBranch& branch) {
        if(!branch.condition.isSet()) return false;

        auto condition = opt.base[branch.condition.value()];
        if(!condition->definer) return false;

        auto test = opt.base[condition->definer];
        if(test->kind != Inst::Binary) return false;

        /*
         * `i < bound` and `bound > i` are the same test written two ways and both are admitted; `<=`
         * is not. Under `le` the counter reaches the bound and steps past it, so the proof would
         * have to rule out a bound at the top of the type - which is the width this pass declines to
         * ask for, and the same refusal the step of one is argued from above.
         */
        auto& compare = (InstBinary&)*test;
        Use counterUse;
        Use boundUse;

        if(compare.op == BinaryOp::Lt) { counterUse = compare.lhs; boundUse = compare.rhs; }
        else if(compare.op == BinaryOp::Gt) { counterUse = compare.rhs; boundUse = compare.lhs; }
        else return false;

        auto counter = valueOf(counterUse);
        auto bound = valueOf(boundUse);
        if(!counter || !bound) return false;

        auto position = argumentIndex(header, *counter);
        if(position == ~Size(0)) return false;

        /*
         * One integer type for the counter, the step and the bound - which is what the proof is
         * stated in, and all of it that is needed. A width is deliberately not asked for: see the
         * file comment, where the difference from `foldableInt` is written down.
         */
        auto type = counter->type();
        if(!type || opt.decl[type]->kind != mir::Type::Int) return false;
        if(bound->type() != type) return false;

        if(!invariantBound(loop, *bound)) return false;

        /*
         * And the step, on every edge that goes back to the header - one per way round, which is
         * what a header with two latches has. An edge from *outside* carries the value the counter
         * starts at and is unconstrained: the proof holds wherever it starts, because the test is
         * what decides whether the loop runs at all.
         */
        auto stepped = false;
        auto ok = true;

        loop.contains.forEach([&](Size index) {
            if(!ok) return;

            auto end = blockAt(U32(index))->terminator(opt.base);
            if(!end) { ok = false; return; }

            forEachEdge(opt.base, *end, [&](Edge& edge) {
                if(!edge.target || opt.base[edge.target] != &header) return;
                if(edge.arguments.size() <= position) { ok = false; return; }

                if(!stepsByOne(*counter, edge.arguments.get(opt.base, position), type)) ok = false;
                stepped = true;
            });
        });

        return ok && stepped;
    }

    /*
     * Conditions 1 to 3 and 5, in one walk, and condition 4 over what it found.
     *
     * The header is the block the test is in and every other block of the loop is on one path from
     * it back to it, so the counter's step runs exactly once per iteration - which is what the
     * argument above assumes and what the terminator rule below establishes.
     */
    Inst* decidedTest(Loop& loop) {
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
                // instruction may not be computed at all - and the one exception is the read the
                // body is made of. A `Store` is not one of them any more: the counter's step is an
                // addition since `promote`, so the only store that could stand here is a write to a
                // slot promotion declined, which is a write this pass has no account of.
                if(isPure(inst.kind) || isTerminator(inst.kind)) continue;
                if(inst.kind == Inst::Load) continue;

                ok = false;
            }
        });

        if(!ok) return nullptr;
        if(!countsToTheBound(loop, *header, branch)) return nullptr;

        return terminator;
    }

    bool run(Loop& loop) {
        auto terminator = decidedTest(loop);
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
         * The lists handed over are therefore only the shape the builder checks, which is why they
         * are each edge's own: since `promote`, a branch's two targets may declare different ones.
         */
        auto boolean = mir::booleanType(opt.program);
        if(!boolean) return false;

        auto never = makeConstant(opt, *terminator, boolean, 0);

        SmallArray<Use, 4> whenTrue;
        SmallArray<Use, 4> whenFalse;
        for(auto used: branch.whenTrue.arguments.contents(opt.base)) whenTrue.push(used);
        for(auto used: branch.whenFalse.arguments.contents(opt.base)) whenFalse.push(used);

        auto editor = opt.ir();
        auto body = opt.base[branch.whenTrue.target];

        editor.replaceTerminator(*blockAt(loop.header),
                                 editor.makeBranchEdges(source,
                                                        editor.use(*never, UseKind::Inspect),
                                                        *body, whenTrue.pointer(), whenTrue.size(),
                                                        *exit, whenFalse.pointer(),
                                                        whenFalse.size()));

        return true;
    }
};

} // namespace

/*
 * A computation whose answer cannot change between iterations, moved to the block in front of the
 * loop, where it happens once.
 *
 * Two kinds of instruction move and the second is the one worth having:
 *
 *  - a **pure** value all of whose operands are defined outside the loop. Free of any question about
 *    order or effect, and mostly what the passes in front of this one leave behind: a `metric`, a
 *    mask, the arithmetic of an index that does not depend on the counter;
 *  - a **read** of an allocation nothing in the loop writes. This is the one the source actually
 *    contains - a bound compared against on every iteration, a field of a record the body only reads
 *    - and it is the reason this pass needs `mayWriteStorage` rather than only a dominance answer.
 *
 * ## Why it is here rather than only one tier down
 *
 * `hoistLoopInvariants` in `lower/lower_licm.cpp` performs the same motion over the lower IR, and
 * README.md's section A assigned this row to it. That verdict was right for the native target and
 * only for it: `doc/proposal/Analysis-IR-JS.md` §6.1 names loop simplification as work the shared
 * OSSA optimizer keeps, because JSIR consumes OSSA and never lowers. What lives only below the fork
 * is not there for JavaScript at all.
 *
 * The two are also not the same pass twice. Down there the questions are about registers and
 * addressing modes - `worthHoisting` refuses a pointer result because an address is a displacement
 * where it stands, and refuses a comparison because its result is the flags. Here the questions are
 * about ownership and about storage a callee may reach.
 *
 * ## What has to be true, and which way of being wrong each rules out
 *
 * **The loop has a preheader in the same region.** `Loops::build` only names a block that leads
 * *only* into the header, so an instruction put there runs exactly when the loop is entered rather
 * than on some path that skips it. The region half is this directory's standing rule in its fifth
 * spelling: a `Continuation` is entered at one point and left at another, so moving an instruction
 * across a boundary is moving it into or out of a frame that runs a different number of times.
 * Nothing inside a `Cleanup` region moves at all, which is the blunt half of the same rule.
 *
 * **The instruction is speculatable**, because the preheader runs when the loop is *entered* and the
 * body runs only if the test passes - so an instruction moved from one to the other happens on a
 * path it did not happen on before, and `while i < n` with `n` zero is that path. A pure instruction
 * is speculatable by the definition of `kSsaPure`: it may be computed again or not computed at all,
 * which is a statement that computing it has no consequence. An `element.addr` is one of those and
 * is worth naming, because the name says access and the instruction does not: it computes an address
 * and reads nothing through it. A `Load` is the one that needs an argument of its own, and it is the
 * allocation: a read of a **contained** frame allocation of this body is a read of storage the frame
 * reserved, which exists whether or not the loop runs. A load through anything else - a global, a
 * pointer out of memory, a borrowed parameter - is a fault the program is entitled to take where it
 * wrote it, and is left alone.
 *
 * **Nothing in the loop writes what a read answers.** `mayWriteStorage` asked of every instruction
 * of every block of the loop, against the allocation the address is rooted in - the same predicate
 * `forwardMemory` and `cse` ask, so a call that was handed the address, an atomic, a drop and a
 * handover all end the candidate here as they do there.
 *
 * **Nothing tracked moves.** No operand and no result may be `Owned` or `Affine`, which is
 * `isRemovable`'s rule and is asked through it; a `Guaranteed` result is declined beside them and it
 * is the tempting one. Hoisting a borrow out of a loop extends its loan over iterations the borrow
 * check never agreed to, which is a decision this stage has no standing to take - the shipping
 * stage's `hoistLoopValues` declines a `Borrow` in as many words and for the same reason.
 *
 * ## Innermost first, and what that buys
 *
 * `Loops` is sorted that way already. A value hoisted out of an inner loop lands in that loop's
 * preheader, which is a block the containing loop holds - so the containing loop's own walk, or the
 * next round of the driver, carries it the rest of the way out. What cannot happen is a value being
 * carried across a loop it was not already inside.
 *
 * The **cost** gate the shipping stage applies to a read out of a nested loop is deliberately not
 * here. Over there it was measured against register pressure in a particular backend
 * ([[yana-inline-limits-vs-llvm]] is the same warning about the inliner's numbers), and this tier
 * has two backends and no register model at all. The lower tier keeps its own gate and applies it to
 * what this leaves, which is where a native cost belongs.
 */

namespace {

struct Hoister {
    OptContext& opt;
    const Contained& contained;
    Dominators& dominators;

    bool inside(Loop& loop, U32 index) { return loop.contains[index]; }

    Value* valueOf(Use used) { return used.isSet() ? opt.base[used.value()] : nullptr; }

    // Whether a value is computed inside the loop - the defining position rather than the kind,
    // which is the one question `Value` answers directly. A constant made by the folder inside the
    // loop is a definition there like any other and is carried out with whatever reads it.
    bool definedInside(Loop& loop, Value& value) {
        if(value.definer) {
            auto block = opt.base[value.definer]->block;
            return block && inside(loop, opt.base[block]->index.index);
        }

        return value.block && inside(loop, opt.base[value.block]->index.index);
    }

    bool operandsOutside(Loop& loop, Inst& inst) {
        auto outside = true;

        forEachUse(opt.base, inst, [&](Use used) {
            auto value = valueOf(used);
            if(!value || definedInside(loop, *value)) outside = false;
        });

        return outside;
    }

    // Whether anything in the loop may write the storage a read answers from - see the file comment.
    bool loopWrites(Loop& loop, Value& allocation) {
        auto written = false;

        loop.contains.forEach([&](Size index) {
            if(written) return;

            auto block = opt.body->block(opt.base, BlockId(U32(index)));
            for(auto pointer: block->instructions(opt.base)) {
                if(mayWriteStorage(opt, contained, *opt.base[pointer], allocation)) written = true;
            }
        });

        return written;
    }

    /*
     * A read this may speculate: a whole load of a **contained** frame allocation of this body,
     * through an address the loop does not compute, that nothing in the loop writes.
     *
     * `Contained` is the speculation half and is exactly the statement wanted: "every use of this
     * address keeps it inside this frame" is what says the bytes are storage the frame reserved, so
     * reading them on the path where the loop runs zero times is a read that cannot fault. A global,
     * a pointer out of storage and a borrowed parameter's pointee have no root here and are left
     * alone, which is the shipping stage's answer at the same question - the fault a program can
     * take through an address it computed is the program's to take.
     *
     * **A projection is admitted**, and that is J41's correction. It used to be whole loads only,
     * measured as "admitting a projected read whose address is computed outside the loop changes not
     * one line of any golden" - true, and about a shape that is not the one this misses. Every read
     * of a container's header has its projections computed *inside* the loop beside the read, so
     * they never reached that test at all; `hoistableChain` below is what carries them out with it.
     * `Contained` is a statement about the allocation, and a field of one is inside the bytes the
     * frame reserved, so the speculation covers a projection exactly as it covers the whole record.
     */
    /*
     * The projection chain a read is reached through, where the whole of it may leave the loop with
     * the read - and empty where the address already stands outside.
     *
     * This is what makes the rule reach a **field**. Every read of a container's header is
     * `field.addr .run`, `field.addr .items`, `load`, and the two projections are computed inside
     * the loop beside the read: a `Matrix` inner loop re-loaded `out.run.items` and `out.length` on
     * every iteration for that reason alone, eighteen instructions against the shipping stage's
     * sixteen, with no fact missing but this one.
     *
     * It is a narrower motion than the trade `movable` refuses, and the difference is what makes an
     * address movable here and not there. Over there an address is a *candidate of its own*, with
     * its readers left inside: `lower_licm.cpp`'s `worthHoisting` measured that at `Matrix` 1.4 ms
     * and `Sort` 5.2 ms, because a displacement in an addressing mode becomes a register live
     * across every iteration. Here a step moves only because a read of it is moving, so what leaves
     * the loop is a chain ending in a value; the steps a *second* read still holds are the same
     * chain, and the next round carries that read out on top of them.
     *
     * An element's **index** has to be outside too, which a field's constant offset is by
     * construction. Nothing else is admitted: a step this walk does not recognize means the address
     * is not a projection of one allocation and the speculation below has nothing to rest on.
     */
    bool hoistableChain(Loop& loop, Value* address, SmallArray<Inst*, 4>& chain) {
        while(address && definedInside(loop, *address)) {
            if(!address->definer) return false;

            auto inst = opt.base[address->definer];
            Value* base = nullptr;

            if(inst->kind == Inst::FieldAddress) {
                base = valueOf(((InstFieldAddress&)*inst).base);
            } else if(inst->kind == Inst::ElementAddress) {
                auto& element = (InstElementAddress&)*inst;
                base = valueOf(element.base);

                auto index = valueOf(element.index);
                if(!index || definedInside(loop, *index)) return false;
            } else if(inst->kind == Inst::Load && soleResult(opt, *inst) &&
                      soleResult(opt, *inst)->convention == Convention::Guaranteed) {
                /*
                 * The **borrow** of the slot, which is how a container's own reads reach it: there
                 * is no `Borrow` instruction at this tier, so `&out` is a `load` whose result is
                 * `Guaranteed` and names the same storage. It is a step of the chain like a
                 * projection, and it moves with the rest.
                 *
                 * Lengthening a loan's extent is the direction an extent rule is not safe in, and
                 * what makes it safe here is the condition the read already owes: **nothing in the
                 * loop writes the allocation**, so no endpoint of the loan is crossed by the motion.
                 * A loan ends at a write to the storage or a destructive read of it, and
                 * `loopWrites` has refused both of those for every block of the loop before this
                 * chain is ever built.
                 */
                base = valueOf(((InstLoad&)*inst).address);
            } else {
                return false;
            }

            // And in the region the motion is confined to. `run` refuses a *block* of another
            // region for the file comment's reason - nothing leaves a cleanup arm or a continuation
            // frame - and a step of the chain leaves one exactly as the read would.
            if(opt.base[inst->block]->region != region) return false;

            chain.push(inst);
            address = base;
        }

        return address != nullptr;
    }

    bool invariantRead(Loop& loop, Inst& inst, SmallArray<Inst*, 4>& chain) {
        if(inst.kind != Inst::Load) return false;

        auto result = soleResult(opt, inst);
        if(!result || result->convention != Convention::Trivial) return false;

        auto address = valueOf(((InstLoad&)inst).address);
        if(!address || !hoistableChain(loop, address, chain)) return false;

        // Whichever value the chain ended at, which is outside the loop by construction. A
        // projection of a contained allocation is inside the bytes the frame reserved, so the
        // speculation argument above covers a field exactly as it covers the whole record.
        auto root = allocationRoot(opt, chain.size() ? valueOf(lastBaseOf(*chain[chain.size() - 1]))
                                                     : address);
        if(!root || !contained[root]) return false;

        return !loopWrites(loop, *root);
    }

    // The base operand of the outermost projection of a chain - the value the walk stopped at.
    Use lastBaseOf(Inst& inst) {
        if(inst.kind == Inst::FieldAddress) return ((InstFieldAddress&)inst).base;
        if(inst.kind == Inst::ElementAddress) return ((InstElementAddress&)inst).base;
        return ((InstLoad&)inst).address;
    }

    // The projections the read being considered has to take with it, filled by `invariantRead` and
    // spent by `run`. A member rather than an out-parameter through `movable`, because it is the
    // same answer read one line later at the one call site.
    SmallArray<Inst*, 4> chain;

    // The preheader's region, which the whole motion stays inside - set by `run` before it scans.
    ContinuationId region;

    // What may move: a pure computation, or the read above. Both owe the ownership rule and neither
    // may be a terminator, which `isPure` already refuses for three of the four kinds and
    // `isTerminator` for the fourth.
    bool movable(Loop& loop, Inst& inst) {
        chain.clear();
        if(isTerminator(inst.kind)) return false;

        auto result = soleResult(opt, inst);
        if(!result || !result->useCount()) return false;

        /*
         * A `Trivial` result and nothing else, which is where this parts company with `cse` two
         * files over on the same question about an `Address`.
         *
         * `lower_licm.cpp`'s `worthHoisting` is the authority and it is a time measurement: an
         * address is a displacement in the addressing mode of whatever reads it, so hoisting one
         * trades a byte of encoding for a register live across **every iteration** of the loop, and
         * it cost `Matrix` 1.4 ms and `Sort` 5.2 ms over there. Measured here it is neither: 742
         * hoists instead of 645 for one lowered line and four at this tier, which is noise against a
         * rule that has a number behind it. What `cse` admits is a different trade - computing an
         * address twice on a path that runs once - and it measured the other way.
         */
        if(result->convention != Convention::Trivial) return false;

        if(isPure(inst.kind)) return isRemovable(opt, inst) && operandsOutside(loop, inst);

        // A read owes the ownership rule by hand, because `isRemovable` opens by refusing every kind
        // that is not pure and a `Load` is deliberately not one - see `inst.def`.
        auto ok = true;
        forEachUse(opt.base, inst, [&](Use used) {
            auto value = valueOf(used);
            if(!value || used.kind() == UseKind::Consume || isTracked(value->convention)) ok = false;
        });

        return ok && invariantRead(loop, inst, chain);
    }

    void run(Loop& loop) {
        if(loop.preheader == Loop::kNone) return;

        auto header = opt.body->block(opt.base, BlockId(loop.header));
        auto preheader = opt.body->block(opt.base, BlockId(loop.preheader));
        if(preheader->deleted || preheader->region != header->region) return;
        if(!preheader->terminator(opt.base)) return;

        region = preheader->region;

        // The loop's blocks in dominator preorder, so that two candidates with a dependency between
        // them arrive at the preheader in that order: a use is dominated by its definition, so the
        // definition's block is visited first and one hoist enables the next in the same walk.
        SmallArray<U32, 32> order;
        loop.contains.forEach([&](Size index) { order.push(U32(index)); });

        for(Size i = 1; i < order.size(); i++) {
            for(Size j = i; j > 0 && dominators.enter[order[j]] < dominators.enter[order[j - 1]]; j--) {
                swap(order[j], order[j - 1]);
            }
        }

        for(auto index: order) {
            auto block = opt.body->block(opt.base, BlockId(index));

            // One region for the whole motion - see the file comment. A block of the loop that is in
            // a region of its own is a cleanup arm or a continuation frame, and nothing leaves one.
            if(block->region != preheader->region) continue;

            for(Size i = 0; i < block->instructionCount();) {
                auto& inst = *opt.base[block->instructionAt(opt.base, i)];
                if(!movable(loop, inst)) { i++; continue; }

                /*
                 * The chain outermost first, which is the reverse of the order it was walked in: a
                 * projection has to arrive behind the base it is taken of.
                 *
                 * A step of it standing in *this* block stands above the read - a use is dominated
                 * by its definition - so each one taken out of here moves the rest of the list down
                 * one, and the cursor has to come back by as many. Left at `i` the walk steps over
                 * that many instructions, which the driver's fixpoint recovers on the next round:
                 * measured over `test/resolve`, what the repair changes is the *order* two candidates
                 * of one block reach the preheader in and not which of them get there. It is here
                 * because a cursor into a list that moved is a bug whether or not this pass happens
                 * to be run again, and one round is cheaper than two.
                 */
                auto editor = opt.ir();
                Size removed = 1;

                for(Size step = chain.size(); step-- > 0;) {
                    if(opt.base[chain[step]->block] == block) removed++;
                    editor.moveInst(*chain[step], *preheader, preheader->instructionCount() - 1);
                }

                editor.moveInst(inst, *preheader, preheader->instructionCount() - 1);
                i -= removed - 1;
            }
        }
    }
};

} // namespace

void hoist(OptContext& opt) {
    auto& loops = loopsOf(opt);
    if(!loops.size()) return;

    Hoister hoister { opt, containedOf(opt), dominatorsOf(opt) };

    // Innermost first, which `Loops` sorted for - see the file comment.
    for(auto& loop: loops) hoister.run(loop);
}

void deadLoops(OptContext& opt) {
    auto& loops = loopsOf(opt);
    if(!loops.size()) return;

    LoopKiller killer { opt, containedOf(opt) };

    // Innermost first, which `Loops` sorted for: an outer loop whose whole body was an inner one
    // becomes removable itself, on the round after the inner one has gone.
    for(auto& loop: loops) killer.run(loop);
}

} // namespace ssa
