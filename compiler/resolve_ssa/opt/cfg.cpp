#include "pass.h"

/*
 * The graph itself: the branches that decide nothing, the blocks nothing reaches, and the block
 * boundaries with nothing on either side of them - README.md's section B, "One `cleanCfg`".
 *
 * Three rewrites in one worklist over blocks, because each of the three produces the shape the other
 * two consume. A folded branch strands an arm; deleting the arm leaves the join it fed with one way
 * in; merging that join back leaves *its* terminator here, which may be another branch on a constant.
 * The shipping stage runs `foldBranches`, `removeUnreachableBlocks` and `mergeBlocks` as three passes
 * and re-runs the group until it settles; here they are one queue that empties.
 *
 * `threadBranches` is at the bottom of this file rather than inside that walk, and section B says
 * why: it *creates* edges, so it cannot run inside a loop holding a block cursor, and the shipping
 * stage deliberately puts it behind if-conversion because a diamond that can be one `select` should
 * be one.
 *
 * ## What is new here, and it is the region rule
 *
 * `Continuation` says which region owns each block, and the verifier holds every edge to it: an edge
 * may stay inside a region, enter a **direct child at its `entry`**, or leave for the `destination`
 * that child declared. Nothing about a diamond's shape says which of its arms is a cleanup, so a
 * pass that reroutes an edge by looking at the graph alone is a pass that can thread a branch
 * straight past the flag test a teardown arm begins with - which is `checkEdgeRegion`'s report and
 * the reason it exists.
 *
 * Every rewrite below therefore takes the same shape of decision, and it is deliberately the blunt
 * one: **a rewrite that moves or removes an edge is confined to one region.** A merge folds a block
 * into a predecessor in the same region; a splice redirects a predecessor to a target in the same
 * region; a thread sends a predecessor to an arm in the same region. Anything crossing a boundary is
 * declined rather than reasoned about, because the only thing that could make it legal is
 * reproducing the verifier's case analysis at every rewrite site, and two statements of one rule is
 * what this codebase spends its comments avoiding.
 *
 * The second half is the two fields. `Continuation::entry` and `Continuation::destination` name
 * blocks, and a rewrite that deleted one of them would leave a region naming a block that is not
 * one. **No block named by either field is ever merged or spliced away** - `isAnchor` below - which
 * is the decision that keeps those fields read-only in this directory. Updating them through the
 * editor was the alternative and it is worse: `entry` is the one point a region may be entered at,
 * so moving it is changing what the region *is*, and there is nothing at a merge site that knows
 * whether that was intended. It also costs nothing measurable, because the two shapes it declines -
 * folding a cleanup arm's first block into the flag test above it, folding a region's rejoin point
 * into the arm that reaches it - are both merges *across* a region boundary that the rule above has
 * already refused.
 *
 * A deleted block is the one exception, and it is not one: an unreachable region is unreachable at
 * its entry, so a live edge can never name a block a sweep took out. See `removeUnreachable`.
 *
 * ## The linear rule, and why an unreachable block owes nothing
 *
 * Deleting a block deletes the instructions in it, and some of those consume owners and some are
 * `Drop`s. That is sound for the reason the verifier's own dataflow gives: the linear rule is a
 * statement about the paths through a body, an unreachable block is on no path, and a `Drop` there
 * runs never. An owned value *defined* in a dead block owes a consumption on the paths that reach
 * its definition, and there are none; an owned value defined in a live block and consumed in a dead
 * one is a value whose consumption was never on a path either, so what the erase removes is an
 * obligation that was already discharged everywhere it existed. What would not be sound is deleting
 * a *reachable* block, which is why the sweep is driven by `reachableOf` and never by "this block
 * looks empty".
 *
 * ## What the shipping stage does here and this does not
 *
 * **`reorderBlocksInRpo` has no counterpart.** Over there, `Block::index` *is* the position in
 * `Function::blocks`, the lowering walks that list in order, and a pass that moves an edge can leave
 * a block listed in front of the blocks defining the values it reads - a wrong number with no
 * diagnostic ([[yana-resolve-cfg-invariants]]). Here `Lowering::lowerBody` takes the shared reverse
 * postorder, which phase 1 changed it to for exactly this reason, so moving an edge owes nothing.
 * That is what makes the splice below affordable at all.
 *
 * **`collapseSinglePhis` has a counterpart, and it is `removeTrivialArguments` at the top of this
 * file.** The merge below answers a block argument by the operand its *one* surviving edge hands it,
 * which is the single-predecessor case and nothing more; a join or a loop header whose several edges
 * all hand over one value is the shape phase 14 found nothing removed, and that is the rewrite the
 * sweep starts with now.
 */

namespace ssa {

/*
 * What `branch (eq/ne %v, K)` proves about its own condition - which value is compared, against
 * what constant, and whether the operator is `==` or `!=`. Shared by `BranchThreader::readTest` and
 * `ConditionDecider::provedBy` below, which both used to walk this same `Inst::Binary` shape on
 * their own before this was one function.
 *
 * Each caller keeps its own extra constraint at its own site rather than the two being merged into
 * one set of rules neither actually wants: `readTest` additionally requires the comparison be
 * defined inside the join block it is testing, because it is answering "does this join's own test
 * name this argument" and a compare from anywhere else is not that join's test even if the shapes
 * match; `provedBy` is walking outward from a branch's condition value with no such requirement, and
 * separately tells a condition that is not a comparison at all (`NotComparison`, where the branch's
 * own `Bool` is what got proved) from one that is a comparison against something that never reduces
 * to a constant (`NoConstantSide`, where nothing is proved) - a distinction `readTest` has no use
 * for, since either failure is the same "not this join's test" to it.
 */
enum class EqualityShape { NotComparison, NoConstantSide, Recognized };

struct EqualityTest {
    EqualityShape shape = EqualityShape::NotComparison;
    Inst* compare = nullptr;
    Value* subject = nullptr;
    U64 constant = 0;
    bool equal = true;
};

EqualityTest recognizeEqualityTest(OptContext& opt, Value& condition) {
    if(!condition.definer) return {};

    auto definer = opt.base[condition.definer];
    if(definer->kind != Inst::Binary) return {};

    auto& compare = (InstBinary&)*definer;
    if(compare.op != BinaryOp::Eq && compare.op != BinaryOp::Ne) return {};

    auto operand = [&](Use used) { return used.isSet() ? opt.base[used.value()] : nullptr; };
    auto lhs = operand(compare.lhs);
    auto rhs = operand(compare.rhs);

    Value* subject = nullptr;
    Maybe<U64> number = Nothing();

    if((number = constantValueOf(opt, rhs))) subject = lhs;
    else if((number = constantValueOf(opt, lhs))) subject = rhs;

    if(!subject || !number) return { EqualityShape::NoConstantSide };

    return { EqualityShape::Recognized, definer, subject, number.unwrap(),
             compare.op == BinaryOp::Eq };
}

namespace {

/*
 * A block argument that says nothing, taken back out: one every incoming edge answers with the same
 * value - or with the argument itself - is that value, and one nothing reads is nothing.
 *
 * ## Why it is one function, and why it is here
 *
 * `promote` carried its own copy, over the arguments *it* had placed, and phase 13's review is what
 * turned that scope into a gap. The `for` desugaring hands the header its bound and its step as
 * block arguments of their own - `bb1(%bound, %step, %counter)`, with a latch that answers each of
 * them with exactly what the header gave it - and nothing at this tier removed one. `mergeInto`
 * answers an argument only where the block has a single predecessor, which a loop header never has;
 * `removeTrivial` declined it because the conversion rather than the promotion had placed it. So
 * `deadLoops` read a bound leaving the loop over the exit edge and a step that was not a `Const`,
 * and kept a loop the shipping stage removes outright. The lower tier's own phi cleanup takes them,
 * which is why nothing was emitted for them natively - but every pass that reads OSSA sees them, and
 * so does the JavaScript backend, which never lowers.
 *
 * One implementation, called from the two places the shape arises: `promote`, at the end of its own
 * run, where the arguments it has just placed are the bulk of what is trivial; and `cleanCfg`, at
 * the top of each sweep, where a header that has just lost its back edge is answered without waiting
 * for a merge a two-predecessor block will never be offered. It lives in this file rather than in
 * one of its own because this is where a block argument is already answered by an operand - the
 * merge below - and the two are the same question asked of one edge and of all of them.
 *
 * ## The rules, and each of them is a refusal
 *
 * **The entry block is never touched.** Its arguments are the function's parameters, the signature
 * declares them, and there is no incoming edge to read an answer off. `promote`'s copy had no such
 * guard and did not need one, since it only ever looked at what it had itself placed; this one walks
 * every block and therefore states it.
 *
 * **`Trivial` and `Address` only.** A tracked argument is a handover point *per edge* - the owner is
 * consumed on each incoming edge and the block owes it onward - so answering it with one of those
 * owners either drops a consumption or duplicates one, and which of the two is a question about the
 * paths through the block rather than about the argument. A `Guaranteed` one is declined for
 * `Editor::replaceAllUses`' loan rule read in the direction that fails: `makeBlockArgument` puts
 * every borrowed block argument on the **caller's** loan, and what replaces it is an ordinary borrow
 * on a body-local one - a substitution that moves a use onto a loan whose endpoints are somewhere
 * else, which is what that assertion refuses. `mergeInto`'s `canSubstitute` admits the same pair the
 * other way round, where the *operand* is the caller's-loan value; nothing here knows which way
 * round it has, so it declines the convention rather than the direction.
 *
 * **A region's `entry` is not excepted**, which is the one place in this file where an anchor is
 * ordinary. The region rule is about edges and this moves none: every edge into the block stays
 * exactly where it is and carries one operand fewer. An argument that one entering edge supplies is
 * redundant on the same terms as any other, and answering it enters the region nowhere new.
 *
 * A worklist rather than a sweep to a fixed point, on `removeTrivialPhis`' measurement. Removing an
 * argument can make two other things trivial and nothing else: one of its own **readers**, which is
 * another block argument an edge was handing it to, and - once its operands stop being read on the
 * edges - one of the values that **fed** it, which may now have no readers at all. Both go back on
 * the list, and the sweep that rescanned every survivor per removal does not have to.
 */
struct ArgumentCleaner {
    OptContext& opt;

    // The queue over values, and the bit that keeps one from being on it twice - `CfgCleaner`'s
    // shape one level down, over `ValueId`. Nothing but a removal puts anything back on it, and
    // there are as many removals as the body has arguments, so it empties.
    Array<U32> queue;
    Size head = 0;

    void push(IndexSet& queued, Value& value) {
        auto index = Size(value.id.index);
        if(index >= queued.size() || queued[index]) return;

        queued.set(index, true);
        queue.push(U32(index));
    }

    // Where a value stands in its block's argument list, and `~Size(0)` for one that has already
    // been taken out - which is how an entry left on the queue by an earlier removal is skipped.
    Size indexOf(Block& block, Value& argument) {
        for(Size i = 0; i < block.argumentCount(); i++) {
            if(opt.base[block.argumentAt(opt.base, i)] == &argument) return i;
        }

        return ~Size(0);
    }

    bool discardableConvention(Convention convention) {
        return convention == Convention::Trivial || convention == Convention::Address;
    }

    /*
     * The one value every incoming edge hands this argument, or null.
     *
     * The argument's own value is not an answer: a loop-carried argument whose latch hands it back
     * unchanged is answered by whatever the *other* edges supply, which is what makes a `for`'s
     * bound and step collapse to what the preheader computed. An edge that supplies nothing, or two
     * that disagree, decline - and the walk is over `incoming` entries rather than predecessors
     * because a predecessor whose two arms both land here owes two answers.
     */
    Value* onlyAnswer(Block& block, Size position, Value& argument) {
        SmallArray<Value*, 8> supplied;
        auto complete = true;

        for(auto pointer: block.incoming(opt.base)) {
            auto end = opt.base[pointer]->terminator(opt.base);
            if(!end) return nullptr;

            forEachEdge(opt.base, *end, [&](Edge& edge) {
                if(!edge.target || opt.base[edge.target] != &block) return;

                auto used = edge.arguments.get(opt.base, position);
                if(!used.isSet()) { complete = false; return; }

                supplied.push(opt.base[used.value()]);
            });
        }

        if(!complete) return nullptr;

        /*
         * The candidate first, over a list of the operands rather than as the walk goes.
         *
         * An operand that *chooses between* this argument and the answer is an answer too - see
         * `choosesBetween` - and whether it is one cannot be decided until the answer is known. So
         * the candidate is the first operand that is neither the argument nor such a choice, and
         * only then is every operand checked against it. Deciding as the walk went made the verdict
         * depend on which edge came first.
         */
        Value* only = nullptr;
        for(auto value: supplied) {
            if(value == &argument || choosesOver(value, argument, 0)) continue;

            only = value;
            break;
        }

        // And the fallback, which is the rule as it stood: where every operand is such a choice
        // there is nothing for them to agree *with*, so the first non-self operand is the candidate
        // and the loop below decides it on identity and constants alone.
        if(!only) {
            for(auto value: supplied) {
                if(value == &argument) continue;

                only = value;
                break;
            }
        }

        if(!only) return nullptr;

        for(auto value: supplied) {
            if(value == &argument || value == only) continue;

            if(sameConstant(only, value)) { byConstant = true; continue; }
            if(choosesBetween(value, argument, only, 0)) continue;
            if(carriesConstant(value, argument, only)) { byConstant = true; continue; }

            return nullptr;
        }

        return only;
    }

    // How deep a chain of choices is followed. Two nested selects is past anything if-conversion
    // builds over one block argument, and the bound is what makes this terminate on a cycle.
    static constexpr U32 kChoiceDepth = 3;

    /*
     * Whether a value is a `select` this argument is an arm of - the operands that cannot answer for
     * themselves and have to wait for a candidate. Read only by the candidate scan above.
     */
    bool choosesOver(Value* value, Value& argument, U32 depth) {
        if(!value || !value->definer || depth > kChoiceDepth) return false;

        auto definer = opt.base[value->definer];
        if(definer->kind != Inst::Select) return false;

        auto& select = (InstSelect&)*definer;
        for(auto used: { select.whenTrue, select.whenFalse }) {
            if(!used.isSet()) return false;

            auto arm = opt.base[used.value()];
            if(arm == &argument || choosesOver(arm, argument, depth + 1)) return true;
        }

        return false;
    }

    /*
     * Whether a `select` between the argument and the answer is that answer - the second rule, and
     * the one a promoted loop-carried value needs.
     *
     * The reasoning is the loop's own induction and is the same one that makes "the argument's own
     * value is not an answer" sound one line up: if the argument is the answer everywhere it is
     * read, then a choice whose every arm is either the argument or the answer produces the answer,
     * whichever way the condition went - so the assumption survives the round trip and is therefore
     * true of every iteration.
     *
     * What produces the shape is `convertSelects` over a promoted slot: an `iter fn`'s outcome
     * signal is written on the arm that yielded and carried unchanged on the arm that did not, so
     * if-conversion turns the two arms into `select(c, Proceed, %signal)` and hands *that* round the
     * loop. Before `promote` reached such a slot the signal lived in storage and
     * `removeDeadStorage` took it; as a value it is a cycle of two live-looking instructions that
     * nothing outside reads, and this is what collapses it.
     */
    bool choosesBetween(Value* value, Value& argument, Value* only, U32 depth) {
        if(!value || !value->definer || depth > kChoiceDepth) return false;

        auto definer = opt.base[value->definer];
        if(definer->kind != Inst::Select) return false;

        auto& select = (InstSelect&)*definer;
        for(auto used: { select.whenTrue, select.whenFalse }) {
            if(!used.isSet()) return false;

            auto arm = opt.base[used.value()];
            if(arm == &argument || arm == only) continue;
            if(sameConstant(only, arm)) { byConstant = true; continue; }
            if(choosesBetween(arm, argument, only, depth + 1)) continue;

            return false;
        }

        return true;
    }

    // How many block arguments deep the carry below follows a cycle. A signal handed round two
    // loops nested inside each other is four, and the bound is the same kind of bound
    // `kChoiceDepth` is: past it the answer is the conservative one.
    static constexpr U32 kCarryDepth = 8;

    /*
     * Whether a **block argument** carries nothing but this answer - `choosesBetween`'s induction
     * written over the other representation a carried value has.
     *
     * `choosesBetween` covers the shape if-conversion leaves, where the choice is a `select` and one
     * value goes round the loop. What it cannot see is the same signal still spelled as *control*:
     *
     *     bb20(%183: I32):              ; the signal, entering as `const 0`
     *       branch %c -> bb24 -> bb23
     *     bb23: jump bb25(%183)         ; not taken, carried unchanged
     *     bb24: jump bb25(%213)         ; taken, written - and `%213` is `const 0` as well
     *     bb25(%185: I32): jump bb20(%185)
     *
     * Neither argument is answered alike by its own edges - `%183` is handed `%185` and `%185` is
     * handed `%183` - so the rule one function up declines both and the pair goes round the loop for
     * ever. It is `filteredChain` in `test/bench/programs/Pipeline.yana` and it is an `iter fn`'s
     * outcome signal on a `for` whose body is guarded: two dead `mov $0x0` per iteration, one of
     * them in the loop.
     *
     * The induction is the one written above and needs nothing new: **assume** every argument the
     * walk is inside answers `only`, and check that assumption against every edge. A set that
     * survives its own round trip is a set whose members are `only` on every iteration, because the
     * values entering it from outside are `only` and nothing inside it produces anything else.
     *
     * **A constant answer only**, which is not the induction's limit but the substitution's: a
     * cycle's members are read in blocks the answer's own definition need not dominate - an edge
     * supplying it may be the *second* way into the cycle - and `byConstant` is what puts a fresh
     * one at the top of the block that reads it. Every caller sets that flag, so the restriction and
     * the repair are one decision.
     */
    bool carriesConstant(Value* value, Value& argument, Value* only) {
        // The two cheap refusals in front of the set, which is `reset` over every value in the body:
        // only a block argument continues this walk, and only a constant may end it.
        if(!value || value->definer || !value->block) return false;
        if(!constantOf(opt, only)) return false;

        ScratchSet assumed(opt.sets, opt.body->values.size());
        if(argument.id.index < assumed->size()) assumed->set(argument.id.index, true);

        return carriesOnly(value, only, *assumed, 0);
    }

    bool carriesOnly(Value* value, Value* only, IndexSet& assumed, U32 depth) {
        if(!value || value == only) return true;
        if(sameConstant(only, value)) return true;
        if(depth > kCarryDepth) return false;

        // A block argument and nothing else continues the walk - the value with a block and no
        // definer, which is `seedOperands`' test one screen down.
        if(value->definer || !value->block) return false;

        auto index = Size(value->id.index);
        if(index >= assumed.size()) return false;
        if(assumed[index]) return true;

        auto block = opt.base[value->block];
        if(block->deleted || block == opt.base[opt.body->entry]) return false;

        auto position = indexOf(*block, *value);
        if(position == ~Size(0)) return false;

        assumed.set(index, true);
        auto carries = true;

        for(auto pointer: block->incoming(opt.base)) {
            auto end = opt.base[pointer]->terminator(opt.base);
            if(!end) return false;

            forEachEdge(opt.base, *end, [&](Edge& edge) {
                if(!edge.target || opt.base[edge.target] != block) return;

                auto used = edge.arguments.get(opt.base, position);
                if(!used.isSet()) { carries = false; return; }
                if(!carriesOnly(opt.base[used.value()], only, assumed, depth + 1)) carries = false;
            });

            if(!carries) break;
        }

        return carries;
    }

    /*
     * Two `const` instructions holding one value, which this IR does not intern - `cse.cpp`'s
     * `sameOperand` states the same rule for the same reason.
     *
     * Without it a block argument every edge answers with zero is not "one answer": the entry edge
     * carries the conversion's constant and the latch carries one a fold made, and identity says
     * they are two values. That is exactly what `decideBranchConditions` leaves behind on the arm it
     * decided, so the two rules are one rewrite read from either end.
     *
     * The substitution then may **not** use either of them - a constant defined in one predecessor
     * does not dominate the join - so `step` makes a fresh one at the top of the block, which is
     * what `byConstant` says it has to.
     */
    bool sameConstant(Value* left, Value* right) {
        auto a = constantOf(opt, left);
        auto b = constantOf(opt, right);
        return a && b && *a == *b;
    }

    // Whether the answer above was reached by comparing constants rather than by identity - see
    // `sameConstant`. Set by `onlyAnswer` and read by its one caller.
    bool byConstant = false;

    // The block arguments an edge was handing this value to - the readers a substitution can make
    // trivial. Taken before `replaceAllUses` moves them off this value's use list.
    void seedReaders(IndexSet& queued, Value& argument) {
        for(auto pointer: argument.uses(opt.base)) {
            auto& user = *opt.base[pointer];
            if(!isTerminator(user.kind)) continue;

            forEachEdge(opt.base, user, [&](Edge& edge) {
                if(!edge.target) return;

                auto target = opt.base[edge.target];
                for(Size i = 0; i < edge.arguments.size() && i < target->argumentCount(); i++) {
                    auto used = edge.arguments.get(opt.base, i);
                    if(!used.isSet() || opt.base[used.value()] != &argument) continue;

                    push(queued, *opt.base[target->argumentAt(opt.base, i)]);
                }
            });
        }
    }

    // The block arguments feeding this position - the values a removal can leave unread. A block
    // argument is the value with a block and no definer, which is the one question `Value` answers
    // without a table.
    void seedOperands(IndexSet& queued, Block& block, Size position) {
        for(auto pointer: block.incoming(opt.base)) {
            auto end = opt.base[pointer]->terminator(opt.base);
            if(!end) continue;

            forEachEdge(opt.base, *end, [&](Edge& edge) {
                if(!edge.target || opt.base[edge.target] != &block) return;

                auto used = edge.arguments.get(opt.base, position);
                if(!used.isSet()) return;

                auto value = opt.base[used.value()];
                if(!value->definer && value->block) push(queued, *value);
            });
        }
    }

    bool step(Editor& editor, IndexSet& queued, Value& argument) {
        if(!argument.block) return false;

        auto block = opt.base[argument.block];
        if(block->deleted || block == opt.base[opt.body->entry]) return false;
        if(!discardableConvention(argument.convention)) return false;

        auto position = indexOf(*block, argument);
        if(position == ~Size(0)) return false;

        Value* only = nullptr;

        if(argument.useCount()) {
            byConstant = false;
            only = onlyAnswer(*block, position, argument);
            if(!only) return false;

            /*
             * A fresh constant at the top of this block, where the edges agreed on a *value* rather
             * than on one instruction - see `sameConstant`. It has to be here rather than in a
             * predecessor: every use of a block argument is in a block this one dominates, so a
             * constant at the top of it dominates all of them and one in an arm dominates none.
             */
            if(byConstant) {
                if(!block->instructionCount()) return false;

                auto constant = constantOf(opt, only);
                auto at = opt.base[block->instructionAt(opt.base, 0)];
                only = makeConstantValue(opt, *at, *constant);
                if(!only) return false;
            }

            // The conventions agree by construction - `checkEdge` compares an edge's operand
            // against the argument it fills - and this is that read back, the way `canSubstitute`
            // reads it back at the merge: an assertion in `replaceAllUses` holds in a checked build
            // and this holds in every one.
            if(only->convention != argument.convention) return false;

            seedReaders(queued, argument);
        }

        seedOperands(queued, *block, position);

        if(only) editor.replaceAllUses(argument, *only);
        editor.removeBlockArgument(*block, position);
        return true;
    }

    void run() {
        queue.clear();
        head = 0;

        ScratchSet queued(opt.sets, opt.body->values.size());
        auto entry = opt.base[opt.body->entry];

        for(auto pointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[pointer];
            if(block->deleted || block == entry) continue;

            for(auto argumentPointer: block->arguments(opt.base)) {
                push(*queued, *opt.base[argumentPointer]);
            }
        }

        auto editor = opt.ir();

        while(head < queue.size()) {
            auto index = queue[head++];
            queued->set(index, false);

            step(editor, *queued, *opt.body->value(opt.base, ValueId(index)));
        }
    }
};

struct CfgCleaner {
    OptContext& opt;

    /*
     * The queue over blocks, and the bit that keeps one from being on it twice - `simplify`'s
     * shape, over `BlockId` instead of `InstId`.
     *
     * A head index rather than a stack for the same reason: the seeding walk's order is the block
     * table's, so a chain of merges collapses in one sweep rather than one per round. Unlike the
     * instruction queue this one never grows past the table it was sized from - nothing here makes
     * a block - but it is written the same way so that the two read alike.
     */
    Array<U32> queue;
    Array<U8> marked;
    Size head = 0;

    void mark(Size index, U8 value) {
        while(marked.size() <= index) marked.push(0);
        marked[index] = value;
    }

    void push(Block* block) {
        if(!block || block->deleted) return;

        auto index = Size(block->index.index);
        if(index < marked.size() && marked[index]) return;

        mark(index, 1);
        queue.push(U32(index));
    }

    void push(SsaPtr<Block> pointer) {
        if(pointer) push(opt.base[pointer]);
    }

    // Everything that reaches this block and everything it reaches - what a rewrite of its
    // terminator or its instruction list has just changed the shape of.
    void pushNeighbours(Block& block) {
        for(auto pointer: block.incoming(opt.base)) push(pointer);

        if(auto last = block.terminator(opt.base)) {
            forEachEdge(opt.base, *last, [&](Edge& edge) { push(edge.target); });
        }
    }

    /*
     * Whether a `Continuation` names this block as its `entry` or its `destination` - the file
     * comment's second half, and the whole of what keeps those two fields out of this directory.
     *
     * The body's own region names the entry block, so this also states "the entry block is never
     * merged or spliced away" without a case of its own - which `Editor::deleteBlock` asserts
     * anyway, and asserting is not the same as declining before the edit.
     */
    bool isAnchor(Block& block) {
        for(auto pointer: opt.body->regions.contents(opt.base)) {
            auto region = opt.base[pointer];

            if(region->entry.valid() && region->entry == block.index) return true;
            if(region->destination.valid() && region->destination == block.index) return true;
        }

        return false;
    }

    // Whether this block is the one point its own region may be entered at. An edge from *inside*
    // the region to it is a re-entry, which is what `Continuation::multiplicity` forbids for a
    // cleanup arm - so a redirect that would produce one is declined at the site rather than
    // reported at the checkpoint.
    bool isRegionEntry(Block& block) {
        auto region = opt.body->region(opt.base, block.region);
        return region && region->entry.valid() && region->entry == block.index;
    }

    // Whether two edges hand their target the same operands. `branch %c, X, X` is legal and the two
    // edges are two independent argument lists, so folding it to one edge is exact only where the
    // two agree - see `foldBranch`.
    bool edgesAgree(Edge& a, Edge& b) {
        if(a.arguments.size() != b.arguments.size()) return false;

        for(Size i = 0; i < a.arguments.size(); i++) {
            if(a.arguments.get(opt.base, i).packed != b.arguments.get(opt.base, i).packed) {
                return false;
            }
        }

        return true;
    }

    /*
     * One terminator built again, detached, from another that is still standing - what a merge needs
     * and the editor deliberately does not have.
     *
     * A terminator cannot be *moved*: `moveInst` refuses one because the edges move with it, and
     * `replaceTerminator` takes a `make*` form precisely because the block being written still holds
     * the one being replaced. So the merge copies the operands into a fresh instruction, places that
     * in the predecessor, and erases the original - which leaves both recorded at their shared
     * successors for the length of one call and neither afterwards.
     *
     * The branch case reads **each edge's own** argument list, which is what `makeBranchEdges` is
     * for. Until `promote` existed there was only one shape - the builder pushes one list onto both
     * edges, which is this tier's rule that a value live before a branch is live on both sides of it
     * - and a merge could therefore copy one list and check the two agreed. Promotion places a block
     * argument where one slot's writes meet, and the two arms of a branch meet different sets of
     * them, so the two edges routinely differ and copying them apart is the only correct thing.
     */
    Inst* cloneTerminator(Editor& editor, Inst& from) {
        switch(from.kind) {
            case Inst::Return: {
                auto& written = (InstReturn&)from;
                SmallArray<Use, 4> values;
                for(auto used: written.values.contents(opt.base)) values.push(used);

                return editor.makeReturn(from.source, values.pointer(), values.size());
            }

            case Inst::Jump: {
                auto& written = (InstJump&)from;
                SmallArray<Use, 4> arguments;
                for(auto used: written.to.arguments.contents(opt.base)) arguments.push(used);

                return editor.makeJump(from.source, *opt.base[written.to.target],
                                       arguments.pointer(), arguments.size());
            }

            case Inst::Branch: {
                auto& written = (InstBranch&)from;
                SmallArray<Use, 4> whenTrue;
                SmallArray<Use, 4> whenFalse;
                for(auto used: written.whenTrue.arguments.contents(opt.base)) whenTrue.push(used);
                for(auto used: written.whenFalse.arguments.contents(opt.base)) whenFalse.push(used);

                return editor.makeBranchEdges(from.source, written.condition,
                                              *opt.base[written.whenTrue.target],
                                              whenTrue.pointer(), whenTrue.size(),
                                              *opt.base[written.whenFalse.target],
                                              whenFalse.pointer(), whenFalse.size());
            }

            /*
             * A multi-way branch - the cases, the fallback, and **each edge's own** argument list.
             *
             * Read per edge for the reason the branch case above is, and it is the same reason: a
             * `switch` is built with one list every arm shares, and `promote` then places a block
             * argument where a slot's writes meet and pushes the operand onto only the arms that
             * reach that block. So a standing `switch` routinely carries different lists, and this
             * read one off the fallback and handed it to every case - which the builder refuses
             * where the counts differ and which would silently re-route a value where they do not.
             * An iterator object's step dispatch is where it was found: three arms into the
             * producer's body, one of them writing a frame slot the others do not.
             */
            case Inst::Switch: {
                auto& written = (InstSwitch&)from;
                auto items = written.cases.pointer(opt.base);
                auto count = written.cases.size();

                SmallArray<I64, 16> values;
                SmallArray<Block*, 16> targets;
                SmallArray<Size, 16> counts;
                SmallArray<Use, 16> operands;

                for(Size i = 0; i < count; i++) {
                    values.push(items[i].value);
                    targets.push(opt.base[items[i].edge.target]);

                    Size own = 0;
                    for(auto used: items[i].edge.arguments.contents(opt.base)) {
                        operands.push(used);
                        own++;
                    }

                    counts.push(own);
                }

                SmallArray<Use, 4> arguments;
                for(auto used: written.fallback.arguments.contents(opt.base)) arguments.push(used);

                return editor.makeSwitchEdges(from.source, written.selector, values.pointer(),
                                              targets.pointer(), counts.pointer(),
                                              operands.pointer(), count,
                                              *opt.base[written.fallback.target],
                                              arguments.pointer(), arguments.size());
            }

            default:
                return editor.makeUnreachable(from.source);
        }
    }

    /*
     * A branch that decides nothing, which is a jump - the first of the three.
     *
     * Two shapes, and the second is not the interesting one. A **constant condition** is what
     * arrives from everywhere else: `while True`, a `match` on a value the caller wrote as a
     * literal, a comparison the folder answered. **Both arms at one block** is what this pass makes
     * itself - the splice below points a predecessor at a target its other arm may already reach -
     * and it has to be folded because the tier below refuses one by name: `validateLowerModule`
     * reports "same target block for all branches".
     *
     * The doubled arm folds only where the two edges hand over the same operands. Two that disagree
     * mean the condition selects between them, which is a `Select` and not a jump, and nothing in
     * this stage builds one - so this declines rather than choosing an arm.
     *
     * `replaceTerminator` is what makes the rest three lines. The surviving edge is in both the old
     * successor multiset and the new one, so it is left exactly as it was, arguments included, and
     * only the untaken arm loses its `incoming` entry. There is nothing at this site that knows what
     * the taken arm was being handed, which is the whole argument for the multiset rule.
     */
    bool foldBranch(Block& block) {
        auto last = block.terminator(opt.base);
        if(!last || last->kind != Inst::Branch) return false;

        auto& branch = (InstBranch&)*last;
        if(!branch.whenTrue.target || !branch.whenFalse.target) return false;

        Edge* taken = nullptr;

        if(branch.whenTrue.target == branch.whenFalse.target) {
            if(!edgesAgree(branch.whenTrue, branch.whenFalse)) return false;

            taken = &branch.whenTrue;
        } else {
            if(!branch.condition.isSet()) return false;

            auto condition = constantValueOf(opt, opt.base[branch.condition.value()]);
            if(!condition) return false;

            taken = condition.unwrap() ? &branch.whenTrue : &branch.whenFalse;
        }

        SmallArray<Use, 4> arguments;
        for(auto used: taken->arguments.contents(opt.base)) arguments.push(used);

        auto target = opt.base[taken->target];
        auto editor = opt.ir();
        editor.replaceTerminator(block, editor.makeJump(last->source, *target, arguments.pointer(),
                                                        arguments.size()));

        push(&block);
        pushNeighbours(block);
        return true;
    }

    /*
     * The same three foldings for a multi-way branch - inventory D3.
     *
     * A **constant selector** is what arrives from the inliner: `nameOf(Red)` opened at a call whose
     * argument is a literal is a `switch` on a number this pass knows, and the answer is one of its
     * own edges. **Every edge at one target** is what this pass makes itself, and the tier below
     * refuses it by name for a branch; a `switch` reaching only one block is a jump for the same
     * reason.
     *
     * The third is **one case left**, which becomes a branch. Nothing produces one today, because
     * the builder refuses to make a `switch` below three cases at all - it is here because that rule
     * is only maintainable if a pass that deletes a case has somewhere to go, and
     * `endNonReturningBlocks` is the pass that will.
     */
    bool foldSwitch(Block& block) {
        auto last = block.terminator(opt.base);
        if(!last || last->kind != Inst::Switch) return false;

        auto& written = (InstSwitch&)*last;
        auto items = written.cases.pointer(opt.base);
        auto count = written.cases.size();
        if(!written.fallback.target) return false;

        Edge* taken = nullptr;

        auto everyEdgeAgrees = true;
        for(Size i = 0; i < count && everyEdgeAgrees; i++) {
            everyEdgeAgrees = items[i].edge.target == written.fallback.target &&
                              edgesAgree(items[i].edge, written.fallback);
        }

        if(everyEdgeAgrees) {
            taken = &written.fallback;
        } else if(written.selector.isSet()) {
            auto selector = constantValueOf(opt, opt.base[written.selector.value()]);
            if(!selector) return false;

            taken = &written.fallback;
            for(Size i = 0; i < count; i++) {
                if(items[i].value == I64(selector.unwrap())) taken = &items[i].edge;
            }
        }

        if(!taken) return false;

        SmallArray<Use, 4> arguments;
        for(auto used: taken->arguments.contents(opt.base)) arguments.push(used);

        auto target = opt.base[taken->target];
        auto editor = opt.ir();
        editor.replaceTerminator(block, editor.makeJump(last->source, *target, arguments.pointer(),
                                                        arguments.size()));

        push(&block);
        pushNeighbours(block);
        return true;
    }

    /*
     * Whether the block argument at one edge may simply *be* the operand that edge hands it - the
     * precondition `Editor::replaceAllUses` asserts, asked before the merge starts rather than
     * discovered in the middle of one.
     *
     * The conventions agree by construction: `checkEdge` compares an edge's operand against the
     * argument it fills and reports a mismatch, so this is the verifier's own rule read back. What
     * is *not* by construction is the loan: a `Guaranteed` argument and a `Guaranteed` operand may
     * depend on different storage, and a borrow rerouted onto a different body-local loan is one
     * whose uses are measured against endpoints in a different place. The caller's loan - an empty
     * storage set, which never ends inside this body - is the one substitution that cannot move an
     * endpoint past a use, and it is admitted for that reason.
     */
    bool canSubstitute(Value& argument, Value& supplied) {
        if(&argument == &supplied) return false;
        if(argument.convention != supplied.convention) return false;
        if(argument.convention != Convention::Guaranteed) return true;

        if(!supplied.loan) return false;
        if(supplied.loan == argument.loan) return true;

        return opt.base[supplied.loan]->storage.size() == 0;
    }

    /*
     * A block with one way in, folded back into the block it leaves from - the second of the three,
     * and the one that pays.
     *
     * What makes the shape is everything else: folding a branch deletes an arm and leaves the join
     * it fed with one predecessor, and phase 9's inliner will cut a caller's block in two and leave
     * the second half with one way in wherever the callee has one `return`. What leaving it costs is
     * every block-local pass below - `forwardMemory` and the block-local half of `simplify` both
     * stop at a boundary that decides nothing.
     *
     * Five guards, and each is a case this must not touch:
     *
     *  - the predecessor ends in a plain **jump**. A branch cannot absorb anything, since the block
     *    would then hold instructions behind its own terminator;
     *  - the block has **exactly one** `incoming` entry, which - the list holding one per edge -
     *    makes that entry this predecessor and makes the concatenation unconditional rather than a
     *    guess about which way control came. `branch %c, X, X` is two entries and is declined here,
     *    correctly: `foldBranch` above is what turns it into one;
     *  - **the same region**, which is the file comment's rule. A block in a child region folded
     *    into its parent would carry that region's blocks' entry point into the parent, and every
     *    edge into what is left would then be entering a region at a block that is not its `entry`;
     *  - **not a region anchor** - see `isAnchor`;
     *  - and each block argument has to be substitutable by the operand the jump hands it, which is
     *    `canSubstitute`.
     *
     * A **loop header is safe by construction rather than by a check**, which is the shipping
     * stage's observation and holds here for the same reason: a header has a back edge, so it has at
     * least two `incoming` entries, so it is never the block being folded. A block that jumps to
     * itself is the degenerate case of the same thing - its one predecessor is itself - and is
     * declined outright, since there is nothing to fold it into.
     */
    bool mergeInto(Block& into) {
        auto last = into.terminator(opt.base);
        if(!last || last->kind != Inst::Jump) return false;

        auto& jump = (InstJump&)*last;
        if(!jump.to.target) return false;

        auto block = opt.base[jump.to.target];
        if(block == &into || block->deleted) return false;
        if(block->predecessorCount() != 1) return false;
        if(block->region != into.region) return false;
        if(isAnchor(*block)) return false;

        auto end = block->terminator(opt.base);
        if(!end) return false;

        // The operands read out before anything moves: `removeBlockArgument` takes each of them off
        // this jump as it goes, so the list this walks is the one it is emptying.
        SmallArray<Use, 4> operands;
        for(auto used: jump.to.arguments.contents(opt.base)) operands.push(used);

        for(Size i = 0; i < operands.size(); i++) {
            if(!operands[i].isSet()) return false;

            auto argument = opt.base[block->argumentAt(opt.base, i)];
            if(!canSubstitute(*argument, *opt.base[operands[i].value()])) return false;
        }

        auto editor = opt.ir();

        // From the last, so that removing one does not shift the ones still to come. Each argument's
        // readers are pointed at the operand first, which is what `removeBlockArgument` requires.
        for(Size i = operands.size(); i-- > 0;) {
            auto argument = opt.base[block->argumentAt(opt.base, i)];
            editor.replaceAllUses(*argument, *opt.base[operands[i].value()]);
            editor.removeBlockArgument(*block, i);
        }

        // The instructions, in order, immediately in front of the jump that is about to go. By
        // `moveInst`, which is an unlink and a relink: re-adding one would register every operand a
        // second time, which reads afterwards as a value with more readers than it has.
        for(;;) {
            if(!block->instructionCount()) break;

            auto inst = opt.base[block->instructionAt(opt.base, 0)];
            if(isTerminator(inst->kind)) break;

            editor.moveInst(*inst, into, into.instructionCount() - 1);
        }

        // And the way out. The copy is placed first and the original erased after it, so the shared
        // successors carry both records for the length of one call - `replaceTerminator` drops the
        // jump into `block`, and the erase drops what `block` itself owned.
        editor.replaceTerminator(into, cloneTerminator(editor, *end));
        editor.eraseInst(*end);
        editor.deleteBlock(*block);

        push(&into);
        pushNeighbours(into);
        return true;
    }

    /*
     * A block holding nothing but a jump, spliced out by pointing every predecessor at its target -
     * the third of the three, and the one that is restricted rather than general.
     *
     * **No block arguments anywhere**, which is three conditions at once: the block declares none,
     * so every edge into it supplies none and fits its target; the target declares none, so the
     * redirect is the same edge at a new address; and there is nothing to merge, so nothing about
     * the shape has to be reasoned about. That is not a convenience. §44 of `test/bench/findings.md`
     * measured the general form on the shipping stage and it **costs**: an empty block in front of a
     * join is where a set of edge copies is *shared*, so splicing it hands every predecessor its own
     * copy and re-creates the critical edge the block was the split of. Restricted to the case that
     * cannot cost, the shipping stage measured it firing on nothing at all - because `mergeInto` had
     * already folded every such block into its predecessor.
     *
     * Here it fires, and the producer is `threadBranches` below. A threaded edge leaves its
     * predecessor holding a jump to somewhere else and nothing else at all, and that block's own
     * predecessor is a branch - which `mergeInto` declines, since a branch absorbs nothing. So the
     * relay stays unless this removes it, and the whole of what threading bought is then one block
     * of indirection deeper than it was.
     *
     * The region rule is the same rule in its third spelling: the target is in this block's region,
     * and so is every predecessor. The second half is not a separate fact - a predecessor in another
     * region could only reach a block that is its own region's `destination` or its child's `entry`,
     * and `isAnchor` has already declined both - but it is checked rather than argued, because the
     * cost is a comparison and the alternative is an argument that a later kind of region has to be
     * re-checked against.
     *
     * A target that is its own region's `entry` is declined outright: every predecessor here is
     * inside that region, so the redirect would be a re-entry, which is what
     * `Continuation::multiplicity` forbids of a cleanup arm.
     */
    bool spliceEmpty(Block& block) {
        if(block.argumentCount() || block.instructionCount() != 1) return false;
        if(!block.predecessorCount() || isAnchor(block)) return false;

        auto last = block.terminator(opt.base);
        if(!last || last->kind != Inst::Jump) return false;

        auto& jump = (InstJump&)*last;
        if(!jump.to.target) return false;

        auto target = opt.base[jump.to.target];
        if(target == &block || target->deleted) return false;
        if(target->argumentCount() || target->region != block.region) return false;
        if(isRegionEntry(*target)) return false;

        for(auto pointer: block.incoming(opt.base)) {
            if(opt.base[pointer]->region != block.region) return false;
        }

        /*
         * Every predecessor pointed at the target instead, taken from the front rather than walked.
         *
         * A redirect removes the entry it acted on - a predecessor whose two arms both land here is
         * two entries and leaves as two - so this reads the list it is emptying. What is tested is
         * the *arm*: a predecessor names this block from at least one of its edges by construction,
         * and stopping where it does not leaves a consistent body (what has moved reaches the target
         * directly, what has not still arrives here) where trusting the invariant leaves a compiler
         * that spins with nothing to say.
         */
        auto editor = opt.ir();
        auto moved = false;

        while(block.predecessorCount()) {
            auto from = opt.base[block.predecessorAt(opt.base, 0)];
            auto end = from->terminator(opt.base);
            if(!end) break;

            Size arm = 0;
            auto found = Size(~Size(0));
            forEachEdge(opt.base, *end, [&](Edge& edge) {
                if(found == ~Size(0) && edge.target && opt.base[edge.target] == &block) found = arm;
                arm++;
            });

            if(found == ~Size(0)) break;

            editor.redirectEdge(*end, found, *target);

            // A predecessor whose other arm already reached the target now names it twice, which the
            // tier below refuses - so it goes back on the queue for `foldBranch`.
            push(from);
            moved = true;
        }

        if(!moved) return false;

        push(target);
        return true;
    }

    /*
     * The blocks nothing reaches, taken out together with the edges they owned into the ones that
     * survive - the file comment's linear-rule note is the argument that this is sound.
     *
     * Three phases, and the order is the whole of it. `Editor::deleteBlock` asserts a block has no
     * `incoming`, and a dead block is very often the predecessor of another dead block - a whole
     * stranded loop, say - so the edges have to go first. The terminators go first for a second
     * reason as well: an edge into a *live* block carries operands, and those uses have to stop
     * counting before the values they name can be erased.
     *
     * Then the instructions, to a fixed point rather than block by block, because a value defined in
     * one dead block is read in another and `eraseInst` refuses an instruction whose results still
     * have readers. A fixed point rather than a topological walk: the order exists - what is dead
     * now was ordinary SSA before the branch that stranded it - so a sweep that removes at least one
     * instruction per round terminates, and writing the order out would be reconstructing a dominance
     * relation that no longer has a meaning over these blocks.
     *
     * Nothing *live* can read a value defined here, which is what makes the whole phase local: a
     * definition has to dominate its readers, and an unreachable block dominates nothing reachable.
     * The one way a live block could name one is over an edge, and those went in the first phase.
     */
    bool removeUnreachable() {
        auto reachable = reachableOf(opt);

        SmallArray<Block*, 16> dead;
        for(auto pointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[pointer];
            if(block->deleted || reachable[block->index]) continue;

            dead.push(block);
        }

        if(!dead.size()) return false;

        auto editor = opt.ir();

        for(auto block: dead) {
            if(auto last = block->terminator(opt.base)) editor.eraseInst(*last);
        }

        for(auto progress = true; progress;) {
            progress = false;

            for(auto block: dead) {
                for(Size i = block->instructionCount(); i-- > 0;) {
                    auto inst = opt.base[block->instructionAt(opt.base, i)];

                    auto unread = true;
                    for(auto resultPointer: inst->results.contents(opt.base)) {
                        if(opt.base[resultPointer]->useCount()) unread = false;
                    }

                    if(!unread) continue;

                    editor.eraseInst(*inst);
                    progress = true;
                }
            }
        }

        for(auto block: dead) editor.deleteBlock(*block);
        return true;
    }

    void runQueue() {
        queue.clear();
        head = 0;
        for(Size i = 0; i < marked.size(); i++) marked[i] = 0;

        for(auto pointer: opt.body->blocks.contents(opt.base)) push(opt.base[pointer]);

        while(head < queue.size()) {
            auto index = queue[head++];
            mark(index, 0);

            auto block = opt.body->block(opt.base, BlockId(index));
            if(block->deleted) continue;

            if(foldBranch(*block)) continue;
            if(foldSwitch(*block)) continue;
            if(mergeInto(*block)) continue;
            spliceEmpty(*block);
        }
    }

    /*
     * The queue and the sweep, alternated until the sweep finds nothing.
     *
     * Two loops rather than one because they enable each other in one direction only: the queue
     * strands blocks, and the sweep - by removing a dead block's edges - leaves the live joins it
     * fed with one predecessor, which is a merge the queue had already declined. Each round of the
     * outer loop deletes at least one block, so it is bounded by the block count and in practice
     * runs twice.
     *
     * The trivial arguments go first in each round, and that position is the whole of what phase 14
     * added here. A header whose back edge the sweep has just removed still declares the arguments
     * that edge answered, and it keeps two predecessors until the merge - so waiting for `mergeInto`
     * is waiting for a shape it never reaches. Running it once in front of the whole loop would
     * answer the arguments the round *arrived* with and none of the ones it makes.
     */
    void run() {
        for(;;) {
            removeTrivialArguments(opt);
            runQueue();
            if(!removeUnreachable()) break;
        }
    }
};

/*
 * One join that a predecessor has already decided, and the edge sent straight past it.
 *
 * The shape is short-circuit boolean control used as a *value*: `let ok = (a && b) || c` converts to
 * a chain of arms that hand a `Bool` to a join, and the join branches on it. On the edge from the
 * arm that wrote `False` the answer is known where the edge leaves, so that predecessor can be
 * pointed at the branch's false arm and never reach the join at all.
 *
 * **A constant on the edge, and nothing else - and that is now a measurement rather than a
 * mechanism.** The shipping stage's `threadBooleanBranches` has two shapes beside this one: a
 * predecessor that ends in a `branch` and hands the join a constant on one of its edges, and one
 * whose own branch condition *is* what it hands over. Both were declined here in phase 5 because
 * `Editor::makeBranch` built one argument list and pushed it onto both edges, so a single arm could
 * not be redirected to a block of another shape. `makeBranchEdges` removed that reason in phase 10,
 * and phase 11 built both shapes on top of it and measured them: **zero firings** over the corpus,
 * and the counter that says why is that of the twelve joins reaching the shape test, **not one** has
 * a predecessor ending in a `branch` at all - forty with if-conversion switched off, and still none.
 *
 * The reason is structural and is one block up. A branch arm never goes straight into a join that
 * declares arguments: the conversion gives every arm a block of its own, and the pass that would
 * remove an empty one is `spliceEmpty` below, which declines a target that declares arguments -
 * §44 of `test/bench/findings.md` measured the general form and it *costs*, because an empty block
 * in front of a join is where a set of edge copies is shared. So the predecessor of a threadable
 * join is always a `jump`, and the constant case is not a subset of the three - it is all of them.
 *
 * ## The join's own test, which is what an `iter fn`'s exit signal arrives as
 *
 * `branch %arg` is the shape a `Bool` join has and it is **not** the shape a continuation's signal
 * has. The desugaring hands a plain integer over - `0` where the body ran on, `1` where it left -
 * and the join compares it: `%c = eq %arg, 0` and `branch %c`. So the join holds two instructions,
 * the branch reads neither of the edges' values directly, and every predecessor still knows outright
 * which arm it selects. Reading the comparison here is what reaches the vector `indexOf`, whose loop
 * carries that integer round the header and re-tests it every iteration - `decideBranchConditions`
 * cannot, because the value is decided at a *merge* rather than under the arm that decided it.
 *
 * What the extra instructions cost is two conditions rather than one. Every instruction the join
 * holds besides its terminator must be **pure**, since a threaded edge skips all of them, and its
 * results must be read **only inside the join**, since a join every predecessor goes round is
 * deleted with everything defined in it. An arm's edge operand defined in the join is refused for
 * the second reason read forwards: it would name, at the predecessor, a value only the join defines.
 *
 * ## The three things that make a redirect legal, and they are the three rules of this tier
 *
 * **The arm's operands have to be computable where the edge leaves.** They are, and the reason is
 * dominance rather than a check: the join holds its branch and nothing else, so the only value
 * defined in it is its one block argument, and that argument's single reader is the condition. Every
 * other operand on an arm's edge is defined outside the join and therefore dominates it - and a
 * definition that dominates a block dominates every predecessor of that block, because a path to the
 * predecessor extends to the join and must pass through the definition. So "the arm's arguments are
 * available at the predecessor" is a consequence of the two shape conditions above rather than a
 * fourth one.
 *
 * **The region rule.** The predecessor, the join and both arms are in one `Continuation`, which is
 * `cleanCfg`'s rule in its fourth spelling; and no arm is its own region's `entry`, since every
 * redirected predecessor is inside that region and an edge from inside to the entry is the re-entry
 * `Continuation::multiplicity` forbids.
 *
 * **The linear rule, and this is the decision.** A join with a **tracked** block argument is not
 * threaded, and neither is one whose arms hand a tracked value over an edge. The first is the
 * blunter of the two and costs nothing: the argument here is the branch condition, and a `Bool` is
 * `Trivial`. The second is the real choice. Duplicating the handover site - the owner is consumed on
 * the predecessor's new edge and still consumed on the join's remaining ones - is sound exactly when
 * the two sites are on disjoint paths, which is a question about the paths through the join rather
 * than about the edge being moved, and there is nothing at this site that answers it. So it is
 * declined, and what that costs is measurable if it ever matters: the arms of a boolean join carry
 * the values the expression around it was in the middle of, and those are the operands of an
 * arithmetic or a comparison.
 *
 * The join's block argument is **not** removed when the last constant edge goes. Either some
 * predecessor still supplies it, in which case it is needed, or none does - and then the join is
 * unreachable, and **this pass deletes it rather than leaving it for the next round's sweep**.
 *
 * That last clause is phase 6's doing and it is worth stating, because the first version left the
 * stranded join standing on exactly the argument above. `verifyBody` checks an unreachable block
 * structurally and dominance is one of the things it checks, so a join left holding `branch %arg`
 * with no way in is reported - the block has no position in the order, so nothing dominates it,
 * including its own argument. It never arose while `forwardMemory` did not exist: a `let` put a
 * store and a load in the join, `thread` requires the join to hold its branch and nothing else, and
 * the joins that did qualify kept a predecessor. Forwarding the load is what makes the whole of an
 * `is` test's join threadable, and `Is.yana`'s `present` is where both of its predecessors go. A
 * pass owes the verifier IR it accepts at its own checkpoint, so the deletion belongs here.
 */
struct BranchThreader {
    OptContext& opt;

    bool isRegionEntry(Block& block) {
        auto region = opt.body->region(opt.base, block.region);
        return region && region->entry.valid() && region->entry == block.index;
    }

    // Whether an arm may be the new target: same region, not a re-entry, and handing over nothing
    // this pass is unwilling to duplicate the handover of - see the note above.
    bool armIsThreadable(Block& join, Edge& arm) {
        if(!arm.target) return false;

        auto target = opt.base[arm.target];
        if(target == &join || target->deleted) return false;
        if(target->region != join.region || isRegionEntry(*target)) return false;

        for(auto used: arm.arguments.contents(opt.base)) {
            if(!used.isSet()) return false;
            if(isTracked(opt.base[used.value()]->convention)) return false;
        }

        return true;
    }

    /*
     * The join's own test, and the two shapes it has - see the note above.
     *
     * `compare` is null where the branch reads the block argument outright, which is the `Bool` join
     * this pass was written for; where it is set, the join holds one equality against a constant and
     * the branch reads that.
     */
    struct JoinTest {
        Inst* compare = nullptr;
        U64 constant = 0;
        bool equal = true;
    };

    Maybe<JoinTest> readTest(Block& join, InstBranch& branch, Value& argument) {
        auto condition = opt.base[branch.condition.value()];
        if(condition == &argument) return Just(JoinTest {});

        if(!condition->definer) return Nothing();

        auto definer = opt.base[condition->definer];
        if(!definer->block || opt.base[definer->block] != &join) return Nothing();

        auto test = recognizeEqualityTest(opt, *condition);
        if(test.shape != EqualityShape::Recognized || test.subject != &argument) return Nothing();

        return Just(JoinTest { test.compare, test.constant, test.equal });
    }

    // Which arm the value a predecessor hands over selects.
    bool selectsTrue(const JoinTest& test, U64 supplied) {
        if(!test.compare) return supplied != 0;
        return (supplied == test.constant) == test.equal;
    }

    // Whether a value is defined by this block - its argument, or an instruction it holds.
    bool definedIn(Block& join, Value& value) {
        if(value.block) return opt.base[value.block] == &join;
        return value.definer && opt.base[value.definer]->block
               && opt.base[opt.base[value.definer]->block] == &join;
    }

    /*
     * Whether everything the join computes stays inside it - the condition the extra instructions
     * cost, in its two halves.
     *
     * A threaded edge **skips** the join, so nothing it holds may be anything but pure; and a join
     * every predecessor goes round is **deleted**, so nothing it defines may be read from outside.
     * The block argument is included in the second half rather than tested by use count, which is
     * what makes "the argument's one reader is the branch" the special case it always was.
     */
    bool isSelfContained(Block& join, InstBranch& branch) {
        for(Size i = 0; i < join.instructionCount(); i++) {
            auto inst = opt.base[join.instructionAt(opt.base, i)];
            if(isTerminator(inst->kind)) continue;
            if(!isPure(inst->kind)) return false;

            for(auto resultPointer: inst->results.contents(opt.base)) {
                auto result = opt.base[resultPointer];
                for(auto userPointer: result->uses(opt.base)) {
                    if(opt.base[opt.base[userPointer]->block] != &join) return false;
                }
            }
        }

        for(Size i = 0; i < join.argumentCount(); i++) {
            auto argument = opt.base[join.argumentAt(opt.base, i)];
            for(auto userPointer: argument->uses(opt.base)) {
                if(opt.base[opt.base[userPointer]->block] != &join) return false;
            }
        }

        /*
         * And the way out: an arm operand this block *computes* would name, at the predecessor a
         * redirect moves the edge to, a value only the join has.
         *
         * An arm operand that is one of the join's own **arguments** is not that, and is the shape
         * §J42 admitted: the predecessor supplied it, so the redirect hands over what it supplied.
         * That is what carries a value across a join whose branch is decided - `findAscii`'s
         * position beside its found flag, which is the whole of the second argument this pass used
         * to refuse a join for having.
         */
        auto outside = true;
        forEachEdge(opt.base, branch, [&](Edge& edge) {
            for(auto used: edge.arguments.contents(opt.base)) {
                if(!used.isSet()) continue;

                auto value = opt.base[used.value()];
                if(argumentIndex(join, *value)) continue;
                if(definedIn(join, *value)) outside = false;
            }
        });

        return outside;
    }

    /*
     * The constant a predecessor hands the join, and nothing where this pass would not move that
     * predecessor at all - the one statement of "this edge is one that goes round", read twice: once
     * by the strand test below and once by the walk that performs the redirect.
     */
    Maybe<U64> movedConstant(Block& join, Block& from, Size tested) {
        auto end = from.terminator(opt.base);
        if(!end || end->kind != Inst::Jump || from.region != join.region) return Nothing();

        auto supplied = ((InstJump&)*end).to.arguments.get(opt.base, tested);
        if(!supplied.isSet()) return Nothing();

        return constantValueOf(opt, opt.base[supplied.value()]);
    }

    // What `from` hands the join as its argument number `index`, or null where it hands over
    // nothing - which `movedConstant` has already refused by the time this is asked.
    Value* suppliedTo(Block& join, Block& from, Size index) {
        auto end = from.terminator(opt.base);
        if(!end || end->kind != Inst::Jump) return nullptr;

        auto supplied = ((InstJump&)*end).to.arguments.get(opt.base, index);
        return supplied.isSet() ? opt.base[supplied.value()] : nullptr;
    }

    // Which of the join's arguments a value is, or nothing where it is not one of them.
    Maybe<Size> argumentIndex(Block& join, Value& value) {
        for(Size i = 0; i < join.argumentCount(); i++) {
            if(opt.base[join.argumentAt(opt.base, i)] == &value) return Just(i);
        }

        return Nothing();
    }

    // How many of a block's `incoming` entries name one predecessor - the edges the join's own
    // branch contributes, which go with it if the deletion below fires. One entry per edge, so a
    // branch whose two arms land on one block is counted twice.
    Size edgesFrom(Block& target, Block& from) {
        Size count = 0;
        for(auto pointer: target.incoming(opt.base)) {
            if(opt.base[pointer] == &from) count++;
        }

        return count;
    }

    /*
     * Whether threading this join would leave one of its arms with no way in - declined, rather
     * than followed by a sweep.
     *
     * It can only happen where **every** predecessor goes round, because that is the state the
     * deletion below fires in and the branch goes with the block: an arm no redirected edge selects
     * and nothing else enters is then unreachable, and an unreachable block is checked structurally
     * - nothing dominates it, so every operand it reads is reported at this pass's own checkpoint.
     * That was `let ok = if c then True else True` in front of an `if ok`, where both predecessors
     * hand over the same literal and the arm the other constant would have selected is reached from
     * nowhere else.
     *
     * Declined rather than swept, because what would have to go is the arm *and everything only it
     * reached*, which is `cleanCfg`'s `removeUnreachable` and not a second copy of it here - and
     * this pass runs behind that one rather than inside it.
     *
     * What the refusal costs is exactly the case it names: a diamond whose two sides wrote the same
     * answer, left as a diamond. Nothing else in this stage collapses one either - the condition is
     * a block argument, so `simplify` never sees a constant, and what would see it is the rule that
     * a block argument every edge supplies one number *is* that number. That rule is not here, and
     * it is the one worth adding if the shape ever measures: with it, the join's branch folds and
     * `cleanCfg` deletes the arm, which is the same answer this declines to reach the wrong way
     * round.
     */
    bool wouldStrand(Block& join, InstBranch& branch, const JoinTest& test, Size tested) {
        Size movable = 0;
        Size toTrue = 0;
        Size toFalse = 0;

        for(Size i = 0; i < join.predecessorCount(); i++) {
            auto constant = movedConstant(join, *opt.base[join.predecessorAt(opt.base, i)], tested);
            if(!constant) continue;

            movable++;
            if(selectsTrue(test, constant.unwrap())) toTrue++; else toFalse++;
        }

        // A join that keeps a predecessor keeps its branch, and both arms keep the edge it owns.
        if(!movable || movable != join.predecessorCount()) return false;

        auto whenTrue = opt.base[branch.whenTrue.target];
        auto whenFalse = opt.base[branch.whenFalse.target];

        if(!toTrue && whenTrue->predecessorCount() == edgesFrom(*whenTrue, join)) return true;
        if(!toFalse && whenFalse->predecessorCount() == edgesFrom(*whenFalse, join)) return true;

        return false;
    }

    bool thread(Block& join) {
        if(!join.argumentCount()) return false;

        auto last = join.terminator(opt.base);
        if(!last || last->kind != Inst::Branch) return false;

        auto& branch = (InstBranch&)*last;
        if(!branch.condition.isSet()) return false;

        /*
         * Which argument the join decides by, and every other one carried across the redirect.
         *
         * §J42 This used to require the join to have exactly one argument, and that was a clause
         * about the *tested* value written as a clause about the block. A second argument is not a
         * second decision: it is a value the join hands onwards, and a predecessor going round the
         * join hands over what it supplied for it. `findAscii`'s scan is the shape - the flag its
         * `iter fn`'s early return leaves and the position beside it arrive at one join, so the flag
         * was decided on both edges and the block was refused for the position's sake. Four
         * instructions of every sixteen-byte step were that refusal.
         *
         * Every argument has to be untracked rather than only the tested one, since a redirect
         * duplicates the *handover* of each of them - which is the note above's second half.
         */
        for(Size i = 0; i < join.argumentCount(); i++) {
            if(isTracked(opt.base[join.argumentAt(opt.base, i)]->convention)) return false;
        }

        // What the join decides its argument by: the argument itself, or one equality against a
        // constant that the join holds. A condition that is neither is a join no incoming edge
        // decides.
        Size tested = 0;
        Maybe<JoinTest> test = Nothing();
        for(Size i = 0; i < join.argumentCount() && !test; i++) {
            test = readTest(join, branch, *opt.base[join.argumentAt(opt.base, i)]);
            tested = i;
        }

        if(!test) return false;
        if(!isSelfContained(join, branch)) return false;

        if(!armIsThreadable(join, branch.whenTrue)) return false;
        if(!armIsThreadable(join, branch.whenFalse)) return false;
        if(wouldStrand(join, branch, test.unwrap(), tested)) return false;

        auto editor = opt.ir();
        auto threaded = false;

        /*
         * Every incoming edge that hands over a constant, sent to the arm that constant selects.
         *
         * The index is not advanced on a rewrite: `replaceTerminator` drops the predecessor's entry
         * from this list, so what was at `i` is now the next one to look at. A predecessor that
         * names the join twice is a `branch`, which is skipped for the reason above, so an entry
         * this walk passes over stays where it is.
         */
        for(Size i = 0; i < join.predecessorCount();) {
            auto from = opt.base[join.predecessorAt(opt.base, i)];

            auto constant = movedConstant(join, *from, tested);
            if(!constant) { i++; continue; }

            auto end = from->terminator(opt.base);
            auto& arm = selectsTrue(test.unwrap(), constant.unwrap()) ? branch.whenTrue
                                                                      : branch.whenFalse;

            // An arm operand that is one of the join's arguments becomes what this predecessor
            // supplied for it, which is the value it would have arrived at the join holding.
            SmallArray<Use, 4> arguments;
            for(auto used: arm.arguments.contents(opt.base)) {
                if(!used.isSet()) { arguments.push(used); continue; }

                auto index = argumentIndex(join, *opt.base[used.value()]);
                if(!index) { arguments.push(used); continue; }

                auto supplied = suppliedTo(join, *from, index.unwrap());
                if(!supplied) { arguments.push(used); continue; }

                arguments.push(Use(SsaPtr<Value>(supplied - opt.base), used.kind()));
            }

            editor.replaceTerminator(*from, editor.makeJump(end->source, *opt.base[arm.target],
                                                            arguments.pointer(), arguments.size()));
            threaded = true;
        }

        /*
         * A join every predecessor has now gone round, taken out here - see the note above.
         *
         * `deleteBlock` is legal in exactly this state and needs nothing arranged: the incoming list
         * is empty because the loop above emptied it, the terminator is the block's only instruction
         * and is erased first, and the argument's only reader was that terminator. An anchor is not
         * excepted, and the sweep's argument is why: a region is entered at its `entry` and left to
         * its `destination`, so a block no live edge names is one no live region names either.
         */
        if(threaded && !join.predecessorCount() && &join != opt.base[opt.body->entry]) {
            editor.deleteBlock(join);
        }

        return threaded;
    }

    void run() {
        // Over a snapshot of the table, because a thread rewrites a *predecessor's* terminator and
        // the walk is over joins - a block visited later may have had its way out changed already,
        // which is the state `thread` reads rather than the one it was seeded with.
        SmallArray<Block*, 32> blocks;
        for(auto pointer: opt.body->blocks.contents(opt.base)) blocks.push(opt.base[pointer]);

        for(auto block: blocks) {
            if(block->deleted) continue;
            thread(*block);
        }
    }
};

} // namespace

void removeTrivialArguments(OptContext& opt) {
    ArgumentCleaner cleaner { opt };
    cleaner.run();
}

void cleanCfg(OptContext& opt) {
    CfgCleaner cleaner { opt };
    cleaner.run();
}

void threadBranches(OptContext& opt) {
    BranchThreader threader { opt };
    threader.run();
}

namespace {

/*
 * A condition a branch has already decided, handed on again from inside the arm it decided.
 *
 * This is **not** the branch-implied *range* facts README.md's two notes measured and did not keep.
 * Those relate two different values - `i < n` proving something about `i` where `n` is read - and
 * cost a lattice. This relates a value to itself: below `branch %c, T, F`, `%c` **is** `True` in
 * everything `T` alone reaches and `False` in everything `F` alone reaches, which is one comparison
 * and no lattice at all.
 *
 * What it is for is the shape an `iter fn`'s early exit leaves. `findAscii` yields out of the middle
 * of its scan, and the desugaring makes that a `found` flag carried round the loop header: the latch
 * hands the header the very value the branch above it just tested false, so the header phis `0` on
 * the entry edge and "the thing that was false" on the back edge, and every iteration re-tests a
 * value that cannot be anything but zero. Rewriting the operand makes both edges the same constant,
 * `removeTrivialArguments` takes the argument out, and the two tests fold - which takes the store of
 * the result out of the loop with them, and with the loop no longer writing storage the invariant
 * lane mask leaves it as well.
 *
 * **Every use in the arm is rewritten**, which is §J63's correction of the narrower rule this was
 * written with. The first version took a terminator's edge operands alone, on the reading that what
 * matters is an operand handed to a block argument - and what actually matters is that the value
 * *is* the constant everywhere the arm alone reaches, which an ordinary operand reads as much as an
 * edge does. `Eq(String).==` is the measurement: it tests the two lengths and then hands its own to
 * `compareMemory`, so against a string literal the count is a known number sitting in an operand
 * slot, and nothing downstream recovers it - the slot is not a block argument, so
 * `removeTrivialArguments` never sees it, and the folder walks from a definition rather than from a
 * branch above a use.
 *
 * The arm's target must have **one** way in, which is what makes "dominated by the target" mean
 * "reached only through this arm". Without it a block dominated by the target could also be entered
 * on a path that never took the branch.
 */
struct ConditionDecider {
    OptContext& opt;
    Dominators& dominators;

    // Whether an instruction names this value at all - either through an ordinary operand slot or as
    // an operand one of its edges carries.
    bool reads(Inst& inst, Value& condition) {
        auto found = false;

        forEachUse(opt.base, inst, [&](Use used) {
            if(used.isSet() && opt.base[used.value()] == &condition) found = true;
        });

        forEachEdge(opt.base, inst, [&](Edge& edge) {
            for(auto used: edge.arguments.contents(opt.base)) {
                if(used.isSet() && opt.base[used.value()] == &condition) found = true;
            }
        });

        return found;
    }

    /*
     * Every reader of the value in every block the arm alone reaches, pointed at the constant.
     *
     * **§J63 - this used to be edge operands alone**, and that was a clause about the shape the pass
     * was first written for rather than about what it knows. What it knows is that the value *is*
     * the constant in these blocks, and an ordinary operand reading it there is as decided as one
     * handed over an edge. The shape that made the difference measurable is a library call whose
     * count argument the caller has just tested: `Eq(String).==` compares the two lengths and then
     * passes its own to `compareMemory`, so against a literal the count is known and the callee was
     * being handed a value instead of a number. Nothing downstream could recover it - the operand is
     * not a block argument, so `removeTrivialArguments` never sees it, and the folder reaches a use
     * from its definition rather than from a branch above it.
     *
     * One constant per block rather than per reader, placed in front of the first one: the readers
     * of a decided value cluster - a call's argument and the comparison the same arm makes of it -
     * and `cse` would merge the copies a reader-at-a-time version made one pass later anyway.
     */
    void decide(Block& block, Edge& arm, Value& condition, U64 answer) {
        auto target = opt.base[arm.target];
        if(!target || target->deleted || target == &block) return;

        // One way in, so that "dominated by the target" is "reached only through this arm".
        if(target->predecessorCount() != 1) return;
        if(!dominators.isReachable(target->index.index)) return;

        for(auto pointer: opt.body->blocks.contents(opt.base)) {
            auto inside = opt.base[pointer];
            if(inside->deleted) continue;
            if(!dominators.dominates(target->index.index, inside->index.index)) continue;

            // Over a snapshot, because the rewrite below inserts the constant into this same list.
            SmallArray<Inst*, 16> readers;
            for(auto instPointer: inside->instructions(opt.base)) {
                auto inst = opt.base[instPointer];
                if(reads(*inst, condition)) readers.push(inst);
            }

            if(!readers.size()) continue;

            // The constant is made only once the scan above has found something to spend it on: one
            // made and left unread is dead IR every golden below this then carries, because this
            // pass runs behind the folder rather than in front of it.
            auto decided = makeConstant(opt, *readers[0], condition.occurrence.type, answer);
            if(!decided) continue;

            auto editor = opt.ir();
            for(auto reader: readers) editor.replaceUsesIn(*reader, condition, *decided);
        }
    }

    /*
     * What one arm proves about one value, where it proves anything.
     *
     * Two shapes, and the second is the one the corpus has. `branch %c` proves `%c` outright on both
     * arms, which is the reading the pass is named for. `branch (%v == K)` proves `%v == K` on the
     * **true** arm and `branch (%v != K)` proves it on the **false** one - which is where an
     * `iter fn`'s exit signal lives: the desugaring compares the signal against `Proceed` and carries
     * the *signal* round the loop, so what is handed on is one comparison away from the branch rather
     * than the branch's own operand.
     *
     * The other arm of an equality proves nothing and is deliberately not guessed at: `%v != K` says
     * which value it is *not*, and this pass installs a value rather than a set of them.
     */
    struct Decided {
        Value* value = nullptr;
        U64 answer = 0;
    };

    Decided provedBy(Value& condition, bool whenTrue) {
        // The condition itself, where a branch is written on a `Bool` the body computed.
        Decided direct { &condition, whenTrue ? U64(1) : U64(0) };

        auto test = recognizeEqualityTest(opt, condition);
        if(test.shape == EqualityShape::NotComparison) return direct;
        if(test.shape == EqualityShape::NoConstantSide) return Decided {};

        auto subject = test.subject;

        // The arm the comparison decided outright.
        if(whenTrue == test.equal) {
            return Decided { subject, test.constant };
        }

        /*
         * And the other arm, where the type has **two** values: "not this one" is "that one".
         *
         * Which is not a general fact and is exactly the shape this pass exists for. An `iter fn`'s
         * exit signal is `Proceed | Leave`, the desugaring branches on `signal == Leave`, and what it
         * carries round the loop is the signal itself along the arm that did *not* leave - so
         * without this row the one operand worth rewriting is the one arm that proves nothing.
         *
         * A payload-free two-constructor record is the only type admitted, and its constructor
         * indices are 0 and 1 by construction: `constantValueOf` answers an `Enum` constant with its
         * constructor index and `makeConstant` builds one back from the same number.
         */
        if(isTwoValued(subject->occurrence.type)) {
            return Decided { subject, test.constant ? U64(0) : U64(1) };
        }

        if(auto other = soleOtherAnswer(*subject, test.constant)) {
            return Decided { subject, other.unwrap() };
        }

        return Decided {};
    }

    // A record of exactly two constructors, neither carrying anything - `Bool`, and every enum an
    // author wrote with two cases.
    bool isTwoValued(DeclPtr<mir::Type> type) {
        if(!type || opt.decl[type]->kind != mir::Type::Record) return false;

        auto& record = *(mir::RecordType*)opt.decl[type];
        return record.cons.size() == 2 && record.fields.isEmpty();
    }

    /*
     * The one value a block argument can hold that is not `excluded`, where every edge answers it
     * with a constant.
     *
     * The type-level rule above says "not this one, so that one" from the declaration; this says the
     * same thing from the *edges*, which is where an `iter fn`'s exit signal actually is. The
     * desugaring hands the signal over as a plain integer - `0` on the arm that ran out, `1` on the
     * arm that yielded and left - so the type is `I32` and says nothing, while the two edges into the
     * block say everything.
     *
     * A `Nothing` for an argument whose edges are not all constants, or where excluding one leaves
     * more than one. Declining the argument's own value on an edge would be a loop-carried answer,
     * and there is none: what this is asked about is a merge of two exits.
     */
    Maybe<U64> soleOtherAnswer(Value& subject, U64 excluded) {
        if(subject.definer || !subject.block) return Nothing();

        auto block = opt.base[subject.block];
        if(block == opt.base[opt.body->entry]) return Nothing();

        Size position = ~Size(0);
        for(Size i = 0; i < block->argumentCount(); i++) {
            if(opt.base[block->argumentAt(opt.base, i)] == &subject) position = i;
        }

        if(position == ~Size(0)) return Nothing();

        Maybe<U64> only = Nothing();
        auto agreed = true;

        for(auto pointer: block->incoming(opt.base)) {
            auto end = opt.base[pointer]->terminator(opt.base);
            if(!end) return Nothing();

            forEachEdge(opt.base, *end, [&](Edge& edge) {
                if(!edge.target || opt.base[edge.target] != block) return;
                if(position >= edge.arguments.size()) { agreed = false; return; }

                auto used = edge.arguments.get(opt.base, position);
                auto number = used.isSet() ? constantValueOf(opt, opt.base[used.value()]) : Nothing();
                if(!number) { agreed = false; return; }

                auto value = number.unwrap();
                if(value == excluded) return;

                if(!only) only = Just(value);
                else if(only.unwrap() != value) agreed = false;
            });

            if(!agreed) return Nothing();
        }

        return only;
    }

    void decideArm(Block& block, Edge& arm, Value& condition, bool whenTrue) {
        auto proved = provedBy(condition, whenTrue);
        if(!proved.value || proved.value->convention != Convention::Trivial) return;

        // Already a constant is one `cleanCfg` folds; one reader is the comparison or the branch
        // itself, and there is nothing else to rewrite.
        if(constantValueOf(opt, proved.value)) return;
        if(proved.value->useCount() < 2) return;

        decide(block, arm, *proved.value, proved.answer);
    }

    void run() {
        SmallArray<Block*, 32> blocks;
        for(auto pointer: opt.body->blocks.contents(opt.base)) blocks.push(opt.base[pointer]);

        for(auto block: blocks) {
            if(block->deleted) continue;

            auto end = block->terminator(opt.base);
            if(!end || end->kind != Inst::Branch) continue;

            auto condition = valueOf(opt, ((InstBranch&)*end).condition);
            if(!condition) continue;

            decideArm(*block, ((InstBranch&)*end).whenTrue, *condition, true);

            // Re-read: the walk above may have rebuilt this block's own terminator.
            auto again = block->terminator(opt.base);
            if(!again || again->kind != Inst::Branch) continue;

            auto& branch = (InstBranch&)*again;
            if(valueOf(opt, branch.condition) != condition) continue;

            decideArm(*block, branch.whenFalse, *condition, false);
        }
    }
};

} // namespace

void decideBranchConditions(OptContext& opt) {
    ConditionDecider decider { opt, dominatorsOf(opt) };
    decider.run();
}

/*
 * The sweep alone, for the one caller that needs it without the queue - `opt/check.cpp`.
 *
 * `dischargeChecks` runs above the `optimizeIr` gate, so in a `-no-checks -no-ir-opt` build nothing
 * else in this directory runs at all and the arm it stranded would stay in the body holding a call.
 * A second reachability walk written there is a second answer to agree with, so what is exported is
 * this one - the cleaner's own sweep, on a cleaner whose queue is never started.
 */
void removeUnreachableBlocks(OptContext& opt) {
    CfgCleaner cleaner { opt };
    cleaner.removeUnreachable();
}

/*
 * A block whose last call does not come back, ended there - §10 item 2 of `test/bench/findings.md`,
 * and the shipping stage's `endNonReturningBlocks` over an **inferred** divergence fact rather than
 * a declared one.
 *
 * Every bounds check and every `@bits` narrowing in `lib/` is `if bad then checkFailed()`, grafted
 * into the caller by `@inline(always)`, so what arrives here is a branch with a call in one arm and
 * both arms jumping to the block below the check. `checkFailed` does not come back -
 * `mir::Summary::diverges`, whose seed is that role - so that second edge is one no execution takes,
 * and everything it costs is paid by the code that does:
 *
 *  - the block below the check is a *join*, which ends every block-local fact. `forwardMemory` stops
 *    there and so does everything else in this directory that reasons within a block;
 *  - a length read by two checks of one index is two loads, because the arm is on a path between
 *    them;
 *  - and the branch survives, since a condition that has not folded is a branch either way.
 *
 * With the edge gone the join has one predecessor and `cleanCfg` - which the driver runs immediately
 * below this - folds all three at once.
 *
 * **The call has to be the last instruction of its block**, exactly as it does in the shipping
 * stage, and here that is a *rule about this pass* rather than one the inliner maintains: what
 * follows a call in the middle of a block is equally dead, and erasing it would mean pointing the
 * readers of everything it defined somewhere, in blocks that are about to become unreachable and
 * have not yet. Ending at the terminator costs nothing and needs no such argument - the check arms
 * this exists for hold a call and nothing else.
 *
 * Values still owned where the block now ends are not a leak and `verifyBody` says so: `Unreachable`
 * is a claim that no execution arrives, so the linear rule has no path there to hold anything to.
 * See the note at its exit check, which is where that reading is argued.
 */
void endNonReturningBlocks(OptContext& opt) {
    auto ended = false;
    auto editor = opt.ir();

    for(auto pointer: opt.body->blocks.contents(opt.base)) {
        auto block = opt.base[pointer];
        if(block->deleted) continue;

        auto terminator = block->terminator(opt.base);
        if(!terminator || terminator->kind == Inst::Unreachable) continue;

        auto count = block->instructionCount();
        if(count < 2) continue;

        auto& last = *opt.base[block->instructionAt(opt.base, count - 2)];
        if(last.kind != Inst::Call) continue;

        /*
         * The summary is the *callee's*, so it may be read only where `callee` is what runs: through
         * a slot or a value it is the signature the call was checked against, which has no body and
         * publishes nothing. `get(false)` is the conservative direction - a caller that believes a
         * diverging callee returns keeps a block, and the other way round it deletes live code.
         */
        auto& call = (InstCall&)last;
        if(!call.callee || call.through != mir::CallThrough::Declaration) continue;
        if(!opt.decl[call.callee]->summary.diverges().get(false)) continue;

        editor.replaceTerminator(*block, editor.makeUnreachable(terminator->source));
        ended = true;
    }

    // The arm that stopped leading anywhere may have been a block's only way in. The rest of the
    // cleanup - a join left with one predecessor, the merge that follows - is `cleanCfg`'s, which
    // the driver runs directly below this.
    if(ended) removeUnreachableBlocks(opt);
}

} // namespace ssa
