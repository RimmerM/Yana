#include "pass.h"
#include "../dominance.h"

/*
 * Inlining: a call replaced by a copy of what it would have done - README.md's section A row for
 * `inlineCalls`, and the only pass here with a cost model.
 *
 * What it buys is mostly not the call. A callee copied into its caller is a body the *other* passes
 * can then see through: `simplify` folds it against the literals the site passed, `forwardMemory`
 * answers the loads of a parameter slot from the store that filled it, `removeDeadStorage` takes the
 * slot, and `cleanCfg` folds the branch a constant condition decided. That is why this runs
 * program-wide *before* the per-function loop rather than inside it - what it leaves behind is work
 * for the seven passes after it.
 *
 * ## The two shapes of copy
 *
 * A **straight-line** callee - one reachable block ending in `return` - is spliced into the caller's
 * own block in front of the call. No block is made, no edge moves, no block argument is declared,
 * and every reader of the call's result is pointed straight at the value the `return` named. That is
 * the common case and it is what keeps `.ssa.opt.expect` readable; it also keeps the callee's
 * `alloc` in the caller's own block, where the block-local half of `simplify` and the per-block scan
 * in `forwardMemory` can still see it.
 *
 * A callee with **control flow** needs the general form: the caller's block is cut in two at the
 * call, the callee's blocks are cloned into the gap, a `join` block is made carrying the result as
 * its one argument, and every cloned `return` becomes a jump to it. The join is a block of its own
 * rather than the second half of the split, because `Editor::addBlockArgument` refuses a block that
 * already holds instructions - a block argument holds on entry and is not a position among them -
 * and `cleanCfg` folds the pair back together immediately afterwards, since the join's one
 * successor has one predecessor.
 *
 * ## What the clone has to map, and it is five things rather than two
 *
 * **Values.** A callee value maps to its clone; a callee **entry-block argument** maps to the call's
 * operand at that position, which is `resolve_ssa/README.md`'s "parameters arrive as entry-block
 * arguments and become storage" read from the caller's side. Every use keeps its `UseKind`: a
 * consuming use of a parameter is a consuming use of the argument, and the call instruction that was
 * consuming it is erased in the same rewrite, so the value is consumed exactly once either way.
 *
 * **Blocks**, with fresh `BlockId`s, cloned in **reverse postorder**. Not in table order: an operand
 * names a value defined in a block that *dominates* its own, and block ids are creation order, so a
 * table-order walk can reach a use before the definition it maps to. Reverse postorder is the order
 * that cannot - which is the same order `Lowering::lowerBody` takes and for the same reason. A block
 * the callee cannot reach is not cloned at all: it is on no path, and its instructions owe nothing.
 *
 * **Loans**, re-minted in the caller's table - `rootLender`, which is where each of the three
 * answers is argued. A `LoanId` is dense per body, so carrying one across is the mistake §8.8
 * refused to make with `mir::LoanId`. What a loan has to be re-minted *on* is caller-side storage,
 * and the callee named three kinds of lender: its own allocations, which map to the clones of them;
 * its parameters, which map to whatever the corresponding **argument**'s own loan names; and its
 * unrooted loan - the one `Editor::addResult` gives every borrow the conversion did not refine -
 * which is the callee saying "somewhere outside" and maps to the union of the borrowing arguments'
 * lenders, with a handed-over `->` argument left out because its storage is released in front of
 * the splice.
 *
 * That union is a may-set and *over*-reports: the loan ends when the first of its lenders is
 * released. Over-reporting is the safe direction for an extent rule, and it is only reached where
 * the callee itself could not be precise - a loan naming a parameter is answered by that parameter's
 * argument alone, so `fn middle(a: x'G, b: y'G, c: z'G) -> y'G` comes back on the second argument's
 * lender and not on all three.
 *
 * This used to take every one of those to the caller's **empty** loan, on the argument that the
 * empty loan is the one substitution `Editor::replaceAllUses` always admits. What that bought was a
 * borrow rooted nowhere where the call's own result was rooted in the caller's slot -
 * `fn tagOf(p: 'Held) -> 'Int = p.tag` is the shape in four tokens - so every use of it landed
 * outside what `verifyBody`'s third rule can measure, and the `drop` of that slot a line below was
 * an endpoint the rule was never measured against. It was also not enough: a callee whose result
 * borrows a *field* of a borrow it took through a pointer roots its loan in a callee **temporary**,
 * which is neither empty nor a parameter, and re-minting that onto the clone handed
 * `replaceAllUses` a body-local loan it refuses outright - `lib/Core/Map`'s three accessors, and
 * `elementField` in `test/resolve/loans/PointeeOrigin.yana`.
 *
 * `Editor::loanOn` canonicalizes the set it is given, which this needs and did not have: the caller
 * reached `{a, b}` from the origin analysis and this walk reaches it from the argument list, and an
 * order-sensitive interning made those two loans over one relationship.
 *
 * **Continuation regions**, re-created under the call site's. A cloned region keeps its kind and its
 * multiplicity; the callee's *root* region maps to the region the call site's block is in, so a
 * cleanup arm inside the callee becomes a cleanup arm nested wherever the call was. `entry` falls
 * out of `Editor::addBlock`, which claims the first block made in a region - and reverse postorder
 * visits a region's entry before any other block of it, since it is the one point control enters at.
 * `destination` is set with `rejoinRegion` from the mapped block.
 *
 * **Allocations stay where they are cloned**, which is inside the caller's own body wherever the
 * callee put them, and that is a decision. `LowerInstAlloca` with a constant size becomes a *frame
 * object* - `collectFrameObjects` in `codegen/x64/frame.cpp` - so an allocation is one slot per
 * instruction however many times control reaches it, and `promoteStackSlots` asks about the stores
 * and loads rather than about where the slot was reserved. A callee inlined inside a loop therefore
 * reuses one slot per iteration, which is correct because each iteration initializes it before
 * reading it and releases it before leaving: that is what the callee's own body already did between
 * its entry and its `return`.
 *
 * ## What is declined, and why each
 *
 *  - **a call that does not name a declaration.** `CallThrough::ClassSlot` and `FunctionSlot`
 *    resolve through something only the lowering can read - a witness slot - so there is no body in
 *    hand. `CallThrough::Value` is the one that is *made* into a direct call first, where this body
 *    is what built the pair: see `devirtualizeSite` below.
 *  - **a callee with no `ssa` body** - a signature, an intrinsic, or a body the conversion declined.
 *  - **an `Erased` generic form.** It is called across the erased ABI with a witness table as a
 *    leading argument, and its body reads its own environment's slots (`gen.value`, a property
 *    projection, a descriptor teardown). Those slots are numbered against *that* declaration's
 *    schema, so a copy of the body inside another function reads the wrong table.
 *  - **recursion**, direct or mutual. Copying a recursive body into a caller copies the recursive
 *    call with it, which is unrolling rather than inlining - a different transformation with a
 *    growth rate nothing here bounds. The call graph's cycles are found once, before the rounds,
 *    which is sound rather than a shortcut: inlining gives a caller edges only to things it could
 *    already reach through the call that went away, so no cycle is made and none is destroyed. The
 *    shipping stage's one exception - `collapsesCycle`, taking one *other* member of the caller's
 *    own cycle - is deliberately not ported: it is what
 *    [[yana-inliner-lens-recursion-runaway]] is open on, where a lens call site inside a
 *    self-recursive function does not converge at all. The same refusal is what makes a *recursive
 *    adaptor chain* stop: a `for` whose body reaches its own function puts the outlined continuation
 *    in the cycle too, since the `fun.value` naming it is an edge of this graph.
 *  - **a callee that may suspend**, read off its published `Summary`. It answers false for every
 *    function today - nothing in this stage is a suspension point - so this is a row rather than a
 *    case, and it is here because the milestone that makes one true is not the milestone that would
 *    think to look here.
 *  - **a `return` in a region that is not the body's own.** A region may be left only for its
 *    declared `destination`, so a `return` inside a cleanup arm would become a jump out of the arm
 *    to somewhere the arm does not rejoin at. Nothing produces one - a cleanup arm is a drop and a
 *    jump back - and it is checked rather than argued because the verifier's rule is the one that
 *    would report it, two passes later.
 *  - **a result the two ends disagree about.** The call's result occurrence is the signature's read
 *    at the call, and the `return` operand's is the same signature's read inside the callee, so they
 *    agree by construction - except where the body's own value is a restatement of the declared
 *    result, which `Conversion::emitLeave` already handles for the `@inline(always)` graft with a
 *    `represent`. The same restatement is made here, and a *tracked* value that disagrees declines
 *    the call rather than being restated, because an owner's declaration and its `return` agree to
 *    the letter and anything else is a defect rather than a coercion.
 *
 * A `Native` is **not** declined. A system call is an ordinary effect, and nothing in a cloned one
 * names the callee's frame: its operands are values, mapped like any other, and the frame it would
 * have addressed is the storage the clone brought with it.
 *
 * ## Adaptor chains, which are the one thing this does in two steps
 *
 * A `for` over an `iter fn` is a **continuation**: `resolve_ssa/convert.cpp` outlines the loop body
 * into a function of its own, builds an environment record of the captures it reads, pairs the two
 * as a `{code, env}` function value and hands it to the iterator, which calls back through it once
 * per element. Nothing about that is removable one half at a time - the indirect call is in a frame
 * that only has the pair as a parameter, and the pair is in a frame that never calls it - so the two
 * alternate:
 *
 *  1. **inline the iterator** into its consumer, which brings the `call [value]` into the frame that
 *     built the pair;
 *  2. **devirtualize** it - `devirtualizeSite`, which rewrites the call to name the declaration the
 *     pair holds, with the environment as argument zero ([[yana-function-values]]: `code` always
 *     takes it first, so a capturing value's declaration already has that parameter and a plain one
 *     reaches its declaration through a thunk that only absorbs it);
 *  3. **inline the continuation**, which brings the next link's `call [value]` in with it.
 *
 * `flattenContinuations` runs that to a fixed point, and `filter(map(each(xs)))` is six of those
 * steps. What each step leaves for the passes below is the other half of the win: the environment
 * record is an aggregate nothing writes, so `simplify`'s `tryAggregateField` answers every capture's
 * read from the component the aggregate was built with, and the record, the pair and the loads
 * around them are then values nothing reads at all - which is what `isDiscardableIfUnread` is for.
 *
 * **Two budgets bound it**, and they are in `Budget` beside the other three because a chain is where
 * the per-callee number stops meaning anything: every link copied brings its own call sites with it,
 * so a chain is a product rather than a sum. `kMaxChainDepth` is how many links one body may absorb
 * and `Budget::chain` is what they may weigh together; `worthInlining`'s `flattening` term weighs
 * each link at `soleBudget` underneath both. [[yana-adaptor-chain-budget]] is the shipping stage's
 * version of the same three-term arrangement.
 *
 * ## The cost model
 *
 * Two numbers and one exemption, where the shipping stage has fourteen terms and a per-target table.
 * That is a starting point rather than a claim: its numbers are calibrated against a particular
 * backend's register pressure ([[yana-inline-limits-vs-llvm]] - budget 20 against LLVM's 50, and
 * raising it is +10.7% because the graft enlarges a loop nest the allocator then spills through),
 * and `test/bench/programs` does not compile under this stage yet, so there is nothing here that
 * could rank a second term honestly. What is measurable is the fixture corpus, and the note in
 * `opt/README.md` is what it measured.
 *
 * **The callee is settled first.** `settleCallee` runs `optimizeBody` on it once, lazily, before any
 * site is weighed against it - [[yana-inline-size-honesty]] is the whole of the argument: a body in
 * the form the conversion wrote holds an `alloc`, a store per write and a load per read for every
 * local, and all of that is gone by the time anything emits it. Judging a body three times its own
 * size is measuring the wrong body, not being conservative.
 *
 * **The size excludes what costs nothing to copy**: a `const` is an immediate at its use, an `alloc`
 * is a frame object rather than an instruction, a terminator is the block boundary the copy already
 * paid for in `blocks`, and a `field.addr`/`element.addr` is address arithmetic the lowering folds
 * into the access it feeds. What is left is the work.
 *
 * **The sole call site is the exemption**, and it is a size win rather than a speed one: a callee
 * this is the only reference to is a body that *moves* rather than one that is copied, and `reach`
 * running after the round is what then takes the original away. "Only reference" is stricter than
 * "only call": a function value naming it, a witness table holding it, and `Function::teardown` all
 * mean the declaration outlives the call, and each is counted.
 *
 * **And the bare `@inline` hint, weighed at the same number.** It is the third thing a declaration
 * says about being copied and the only one of the three that is not a directive - it says "weigh
 * this callee as if it were smaller", never "copy this body", so every structural refusal above and
 * the ceiling below still apply. `soleBudget` is what it is weighed at because that number is
 * already this file's statement of "a body this large is worth copying where there is a reason to",
 * and a hint is a reason; a third budget between the two would be a number with nothing behind it.
 * `Size` declines it, which is `limits.budget` being zero.
 *
 * ## The two terms the shipping stage has that this does not, each measured rather than argued
 *
 * **No accessor bonus.** [[yana-accessor-inline-bonus]] is 6 at Balanced and 12 at Speed for a body
 * whose whole content is an address crossing the call boundary, and its two callees were size 7 and
 * 10 against a limit of 6 and 7. Here a body of twenty or under is admitted whatever shape it has,
 * so an accessor cannot be declined for its size: counted over both corpora, the smallest callee
 * this pass declines that is one block with a load and no call is **21** instructions, which is not
 * an accessor by any reading. The flat budget covers the term.
 *
 * **No block-count cap.** `maxBlocks` is a hard cap over there beside `blockCost`, and what it
 * prices is the *graft* blocking the passes downstream - a callee spliced as five blocks leaves its
 * `alloc` behind a branch where forwarding stops answering. Here `promote` runs first in the
 * per-function list and answers most of that, and the corpus says so: a cap at 32, 24, 16 and 8 is
 * a regression at every setting, +3, +7, +39 and +828 lowered instructions, for at most 23 fewer
 * blocks at the setting that costs the most. Measured in phase 16 and not kept.
 */

namespace ssa {

namespace {

/*
 * A cap on the cascade rather than a termination proof, on the same terms as the driver's own round
 * limit. A chain of callees each of which calls the next collapses one level per round; recursion is
 * refused outright, so what is left for this to bound is a call depth deeper than three, which stops
 * being inlined rather than going wrong.
 */
constexpr Size kMaxInlineRounds = 3;

/*
 * How many links of an adaptor chain one body may absorb - `flattenContinuations`.
 *
 * Eight, which is a three-stage chain and one to spare: each stage costs two (the iterator's body,
 * then the continuation the devirtualized call names), and the consumer's own continuation is the
 * last of them. This is a cap on the *cascade* and not the thing that keeps the pass finite - the
 * weight budget below and `clonableDeclaration`'s refusal of a cycle are - so raising it grows what
 * is reachable and never what is possible.
 */
constexpr U32 kMaxChainDepth = 8;

/*
 * What a callee may cost, by `settings.inlining`.
 *
 * Three numbers, and the ceiling is the one that is not a budget: it is the size past which the sole
 * call site stops helping, so that a body of any size is not copied on the strength of one reason.
 * `soleBudget` is read by two rules rather than one - the sole call site and the `@inline` hint -
 * because both are "there is a reason to copy this" and pricing them apart would be inventing a
 * difference the corpus cannot rank.
 *
 * `budget` at `Balanced` is twenty, which is the shipping stage's `Speed` number rather than its
 * `Balanced` eight. The two are not comparable and taking the larger is the honest translation:
 * over there the eight is a base that fourteen bonuses are added to and four penalties subtracted
 * from, and here there are none - a callee is weighed on its size and on whether it is the only
 * reference to its body. What the number cannot be here is *measured*: `test/bench/programs` does
 * not compile under this stage, so the corpus that ranks a budget is out of reach and the note in
 * README.md says so rather than implying a measurement.
 */
struct Budget {
    // What a callee may cost to be copied wherever it is called.
    U32 budget = 0;

    // And what it may cost where this is the only reference to it - a *second budget* rather than a
    // bonus added to the first, because the two price different things: one is a copy and the other
    // is a move.
    U32 soleBudget = 0;

    U32 ceiling = 0;

    /*
     * And what a whole adaptor chain may weigh in one body, which is a budget the other three cannot
     * express: they price one copy, and a chain is a product - every link copied brings its own call
     * sites with it, so a per-callee number bounds each of eight levels and nothing bounds the eight.
     *
     * Zero at `Size` for that level's one sentence, and it is the same zero `budget` carries: a
     * chain flattened is a program grown.
     */
    U32 chain = 0;
};

/*
 * What a body may weigh at a **cold** site - see `worthInlining`, where the argument is.
 *
 * One instruction, and it is a statement rather than a level: `sizeOf` counts a call as one, so a
 * callee of one instruction replaces a call with a call and the program cannot grow. Not in
 * `Budget`, because a budget is a trade between size and speed and this is the one place there is
 * nothing to trade - the author has said the path is rare, so speed is not what the bytes buy.
 */
constexpr U32 kColdBudget = 1;

/*
 * A **repeated-callee penalty was built here, swept and not kept** - `test/bench/findings.md` §81,
 * and it is worth the paragraph because the premise it was built on had already been removed.
 *
 * The term was the shipping stage's `repeatedPenalty` restated for the two-number model: `budget`
 * shrinks by a fixed amount per copy of one callee already made *into this caller*, so the sixth
 * `push(stack, ...)` spliced into `Sort`'s `sort` is priced differently from the second. §78.7's
 * item 4 asked for it on the strength of `Sort` at 1.395x code and 1.162x wall against the shipping
 * stage at 0.997x instructions - a pure instruction-cache regression.
 *
 * Swept at 0, 1, 2, 3, 4, 5, 6, 8, 12 and 20 over the thirteen-program corpus. Code: 24572 bytes at
 * zero, best at **24403** (penalty 2) and 24415 (penalty 3), all of it `Sort`, and above 4 it grows
 * again - a refused copy leaves the callee *and* the call. Wall time on `Sort`, §78's protocol:
 * **0.986x** of the shipping stage at zero, 0.997x at 2 and **1.158x** at 3. `Hash` does not move at
 * any value.
 *
 * So the term buys 169 bytes for 1.1% of `Sort`'s wall time, and falls off a cliff one step further
 * along. What made it not worth keeping is the first number: `Sort` is 0.986x here, not 1.162x -
 * `endNonReturningBlocks` running in front of the inliner took the joins and the phis out of every
 * checked subscript, and the regression the term was for went with them. A number that trades time
 * for bytes on a program that is already faster than what it is compared against is a number with
 * nothing behind it.
 */
Budget budgetFor(InlineLevel level) {
    switch(level) {
        case InlineLevel::None:
            return Budget { 0, 0, 0, 0 };

        // Only the case that cannot grow the program, which is why the budget is zero: a callee
        // qualifies here through the sole call site and through nothing else.
        case InlineLevel::Size:
            return Budget { 0, 96, 96, 0 };

        case InlineLevel::Balanced:
            return Budget { 20, 76, 96, 240 };

        case InlineLevel::Speed:
            return Budget { 40, 120, 160, 480 };
    }

    return Budget();
}

/*
 * How many times a declaration is named, split by whether the reference is a call this pass could
 * remove - `countReferences`.
 *
 * The split is the whole of what the sole-call-site rule rests on. A direct call is a reference that
 * goes away when the call is spliced; a function value, a witness table entry and being a teardown
 * are references the lowering follows and this pass cannot see removed, so a callee with one of them
 * is one whose body stays whatever happens to the call.
 */
struct References {
    U32 calls = 0;
    U32 other = 0;
};

/*
 * The mapping one clone carries - `cloneBodyInto`'s state.
 *
 * Four tables indexed by the callee's own dense ids, which is what makes every lookup an array read
 * rather than a hash: `ValueId`, `BlockId`, `LoanId` and `ContinuationId` are all dense per body.
 */
struct Clone {
    Body& source;

    Array<SsaPtr<Value>> values;

    // Which of those are the callee's **parameters**, which map to the caller's own values rather
    // than to a clone. `mintLoans` is the reader: a borrowed argument's loan was decided where the
    // argument was made and is not this body's to re-mint.
    Array<bool> parameters;

    /*
     * What "somewhere outside the callee" is, named in the **caller**: the storage every borrowing
     * argument's own loan is rooted in, unioned - `bindParameters` fills it and `mintLoans` is the
     * reader.
     *
     * A may-set, and it has to be: the callee's unrooted loan is not one lender (`Editor::addResult`
     * gives it to every borrow the conversion did not refine), so a callee with two borrowed
     * parameters has one loan covering both and picking either is picking wrong for the other. The
     * union over-reports - the loan then ends when the first of them is released - which is the safe
     * direction for an extent rule, where under-reporting is what lets a use past an endpoint.
     *
     * **A handed-over argument is left out.** A `->` operand is consumed at the call, so its storage
     * has already been released in front of the body that is being spliced in and naming it here
     * would put an endpoint before every use of the clone.
     */
    Array<SsaPtr<Value>> outside;

    Array<SsaPtr<Block>> blocks;
    Array<SsaPtr<Loan>> loans;
    Array<ContinuationId> regions;

    // The callee's reachable blocks, in the order they are cloned - see the file comment.
    Array<U32> order;

    // Where a cloned `return` goes, and what it hands over. Null in the straight-line case, which
    // has no edge at all.
    Block* join = nullptr;
    Value* result = nullptr;
};

struct Inliner {
    OptContext& opt;
    Budget limits;

    // Which functions are in a call-graph cycle - `findRecursion`, computed once before the rounds.
    HashMap<U32, bool> recursive;

    // Per callee, this round's reference counts and whether its body has been settled.
    HashMap<U32, References> references;
    HashMap<U32, bool> settled;

    Array<SsaPtr<Inst>> pending;

    SsaBase base() { return opt.base; }
    DeclBase decl() { return opt.decl; }

    /*
     * The callee, folded to a fixed point before any site is weighed against it - and the caller's
     * position given back afterwards.
     *
     * Lazily and once. This pass walks every function of every module, Core and the prelude
     * included, and a program reaches a few dozen of them; settling only the ones something calls is
     * what keeps this from being the driver's own loop run twice. What it costs on top is nothing,
     * because the driver optimizes each of those bodies afterwards anyway and now starts from a
     * fixed point.
     *
     * `opt.function` and `opt.body` are the caller's and are restored, which is not a detail: the
     * analysis caches are stamped with the function they were read from, so a settle in the middle
     * of a walk that left the wrong one current would hand the caller a dominator tree of the
     * callee's.
     */
    void settle(mir::Function& function) {
        if(!function.ssa) return;

        auto caller = opt.function;
        auto body = opt.body;

        opt.function = &function;
        opt.body = base()[function.ssa];
        optimizeBody(opt);

        opt.function = caller;
        opt.body = body;
    }

    void settleCallee(DeclPtr<mir::Function> pointer) {
        auto entry = settled.add(pointer.offset);
        if(entry.existed && *entry.value) return;

        *entry.value = true;
        settle(*decl()[pointer]);
    }

    /*
     * What a body costs to copy, in instructions - see the file comment for what is excluded and
     * why each of them is.
     *
     * Over the reachable blocks only, which is the set the clone actually copies.
     */
    U32 sizeOf(mir::Function& function) {
        auto body = base()[function.ssa];

        Array<U32> order;
        reversePostorder(base(), *body, order);

        U32 size = 0;

        for(auto index: order) {
            auto block = body->block(base(), BlockId(index));

            for(auto pointer: block->instructions(base())) {
                switch(base()[pointer]->kind) {
                    case Inst::Const:
                    case Inst::Alloc:
                    case Inst::FieldAddress:
                    case Inst::ElementAddress:
                    case Inst::Return:
                    case Inst::Jump:
                    case Inst::Branch:
                    case Inst::Unreachable:
                        break;

                    /*
                     * A multi-way branch is **not** free, and it is not one instruction either: it
                     * expands to a comparison and a branch per case unless the backend finds a table
                     * for it. Understating it is how the inliner admits a body that then explodes.
                     */
                    case Inst::Switch:
                        size += U32(((InstSwitch&)*base()[pointer]).cases.size());
                        break;

                    default:
                        size++;
                        break;
                }
            }
        }

        return size;
    }

    /*
     * The caller's **cold blocks** - `@cold`'s second half, and the half `@noinline` could not state.
     *
     * A block is cold if it calls a `@cold` function, or if every one of its successors is: the
     * relation is "control that arrives here reaches nothing but a cold call before it leaves", and
     * that is a backward walk to a fixed point rather than a property of one instruction.
     *
     * **An assertion is the shape it is for.** `check`, `checkEqual` and every `require*` in
     * `lib/Test/Check.yana` are a test and a failure arm, and the arm builds an interpolated message
     * - a `String` allocation, a `push` per piece, a `show` per value - before calling `record`.
     * Marking `record` `@noinline` says nothing about any of that: the message is built in the
     * *caller's* block, so it is the caller's budget that pays for it, and `test/base` is 319
     * assertion-heavy tests. This is what says it: nothing is expanded into a block whose whole
     * purpose is to report a failure.
     *
     * The two ends of the rule are deliberate. A block with **no** successors is cold only if it
     * makes a cold call itself - a `return` block is where the ordinary path ends too, and calling
     * every leaf cold would walk backwards over the whole body. And a block with *some* cold
     * successors is not cold: an `if` whose failure arm reports is a block that leads somewhere warm
     * as well, and the arm is what is rare rather than the test.
     *
     * Recomputed per caller per round rather than cached, because a splice makes blocks - and read
     * with a bound rather than an assertion for the same reason: a call the flattening pass reaches
     * may stand in a block that did not exist when this ran, and the honest answer for one of those
     * is "not known to be cold".
     */
    Array<bool> coldBlocks;

    void findColdBlocks(Body& body) {
        coldBlocks.clear();
        for(U32 i = 0; i < body.blocks.size(); i++) coldBlocks.push(false);

        auto callsCold = [&](Block& block) {
            /*
             * **"A block control cannot leave is cold" was built and measured out**, and it is
             * written down because it is the obvious next rule.
             *
             * `endNonReturningBlocks` runs in front of this pass and leaves an `Unreachable` where a
             * diverging call was, so every bounds check's failure arm, every `assert` that does not
             * hold and every `checkFailed` would be covered with nothing in `lib/` annotated. It
             * costs more than it buys: **+1,392 bytes over the 318 `test/resolve` executables and
             * +156 over the thirteen against +400 on `test/base`**, at a cold budget of one, and
             * raising that budget to eight recovers the first two and gives back 2,480 of `test/base`.
             *
             * The reason is the leaf. An abort helper is three to eight instructions with a caller
             * at every check, so refusing the copy leaves the call *and* an out-of-line body in a
             * program that has one check - where inlining it left nothing at all. So the attribute
             * is for the *arm*, not for the leaf at the end of it, and `checkFailed` is deliberately
             * not `@cold` for the same reason.
             */
            for(auto pointer: block.instructions(base())) {
                auto inst = base()[pointer];
                if(inst->kind != Inst::Call) continue;

                auto& call = (InstCall&)*inst;
                if(call.through != mir::CallThrough::Declaration || !call.callee) continue;
                if(decl()[call.callee]->cold) return true;
            }

            return false;
        };

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base(), BlockId(i));
            if(block->deleted) continue;

            // The flag the last round left, and the evidence this one can still see. Both, because
            // the evidence is what the pass removes: see `Block::cold`.
            if(block->cold || callsCold(*block)) coldBlocks[i] = true;
        }

        auto changed = true;
        while(changed) {
            changed = false;

            for(U32 i = 0; i < body.blocks.size(); i++) {
                if(coldBlocks[i]) continue;

                auto block = body.block(base(), BlockId(i));
                if(block->deleted) continue;

                auto successors = 0u;
                auto warm = false;
                forEachSuccessor(base(), *block, [&](U32 target) {
                    successors++;
                    if(target >= coldBlocks.size() || !coldBlocks[target]) warm = true;
                });

                if(successors && !warm) { coldBlocks[i] = true; changed = true; }
            }
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            if(coldBlocks[i]) body.block(base(), BlockId(i))->cold = true;
        }
    }

    bool inColdBlock(SsaPtr<Block> pointer) {
        if(!pointer) return false;

        auto index = base()[pointer]->index.index;
        return index < coldBlocks.size() && coldBlocks[index];
    }

    /*
     * Whether the callee's shape is one this pass reproduces - the file comment's list, asked once
     * per site because every clause is about the declaration rather than about the site.
     */
    bool clonableDeclaration(mir::Function& caller, DeclPtr<mir::Function> pointer) {
        auto callee = decl()[pointer];

        if(callee == &caller) return false;
        if(!callee->ssa || !callee->reachable) return false;

        // A generic compiled once, which reads something the copy cannot bring with it: a slot of
        // the callee's own environment, numbered against that declaration's schema.
        if(callee->form == mir::GenForm::Erased && mir::isGeneric(decl(), callee->gen)) return false;

        // A suspension has no representation at this tier, so this is a row rather than a case - see
        // the file comment.
        if(callee->summary.suspension().get(true)) return false;

        return !recursive.getValue(pointer.offset);
    }

    /*
     * And the two clauses that are about the body rather than about the declaration - asked after
     * `settleCallee`, since what is being weighed and copied is the settled form.
     */
    bool clonableBody(mir::Function& callee) {
        auto body = base()[callee.ssa];
        auto region = base()[body->entry]->region;

        for(U32 i = 0; i < body->blocks.size(); i++) {
            auto block = body->block(base(), BlockId(i));
            if(block->deleted) continue;

            for(auto instPointer: block->instructions(base())) {
                auto inst = base()[instPointer];

                // A `return` that is not in the body's own region - the file comment's clause, and
                // the one thing about a *callee* that the region rule can refuse.
                if(inst->kind == Inst::Return && block->region != region) return false;

                // A read of the callee's own generic environment. Unreachable given the erased
                // refusal above, and checked because the two are different statements.
                if(inst->kind == Inst::GenValue) return false;

                if(inst->kind == Inst::FieldAddress &&
                   ((InstFieldAddress&)*inst).property != InstFieldAddress::kNoProperty) {
                    return false;
                }
            }
        }

        return true;
    }

    /*
     * Whether the copy is worth making - the two numbers and the exemption.
     *
     * `@inline(always)` reaching here at all means the conversion's graft declined it, which happens
     * at eight expansions deep and nowhere else. It is **weighed against nothing** - J49, and the
     * two reasons are that it is a directive and that the graft is the other half of the same one.
     *
     * The graft applies no size test whatever, so a ceiling here made the directive mean one thing
     * for a call the conversion reached and another for a call eight deep that it did not. Worse, it
     * made it mean *nothing at all* at `-inline none`, where the ceiling is zero and the early-out
     * above refused every callee before the attribute was read - and `-inline none` is a supported
     * way to build, which is the whole argument `resolve/inline.h` makes for the other stage's graft
     * living in the resolver: a body whose cost depends on an optimization flag is the thing the
     * directive exists to prevent.
     *
     * What keeps the pass finite is not this number. `clonableDeclaration` refuses a call-graph
     * cycle and `kMaxInlineRounds` caps the cascade at three, and `checkAlwaysInline` refused a
     * self-call where the attribute was written. So this is the cap on a *cascade* coming out from
     * under a rule that was never a weight.
     *
     * `flattening` is the one term that is about the *site* rather than about the callee - a link of
     * an adaptor chain, weighed at `soleBudget` because what the copy buys is not the body: it is
     * the indirect call on the other side of it becoming a direct one. See `flattenContinuations`,
     * where the chain's own two budgets are.
     */
    bool worthInlining(DeclPtr<mir::Function> pointer, bool flattening, bool cold) {
        auto callee = decl()[pointer];

        // `@noinline`, which is a directive rather than a weight: declining to inline is always
        // possible, so this is the one thing a declaration says here that is never traded against a
        // size. Asked before the numbers for that reason, and not after them.
        if(callee->noInline) return false;

        /*
         * And the two check roles under `-check-locations`, which is the same shape of rule.
         *
         * The located pair is substituted for the unlocated one in the **lowering**
         * (`Lowering::locatedCheck`), so a call that has been expanded by then is a check with no
         * location left to give. Refusing the expansion is what keeps that substitution the one
         * place the feature lives; the alternative is emitting the located form up in MIR, where
         * every rule that reads a check would have to learn a second shape.
         *
         * The cost is a call per check site in a build that asked for locations, which is a debug
         * switch, and it is the same call the shipping stage's located build makes.
         */
        if(opt.context.settings.checkLocations &&
           (pointer == opt.program.roles.function(intrinsic::Known::checkFailed) ||
            pointer == opt.program.roles.function(intrinsic::Known::checkCondition))) {
            return false;
        }
        // And the directive on the other side, above every number for the reason the note has.
        if(callee->alwaysInline) return true;

        if(!limits.ceiling) return false;

        auto size = sizeOf(*callee);
        if(size > limits.ceiling) return false;

        /*
         * The **sole call site**, which is a move rather than a copy: the callee's last reference
         * goes with the splice and `markProgramReachable` drops the body, so what the program pays
         * afterwards is the copy instead of the body rather than as well as it.
         *
         * A lambda because a cold site reads it too and reads nothing else - see below. It is the
         * one term `InlineLevel::Size` keeps for exactly this reason, and its comment is the
         * argument.
         */
        auto solelyReferenced = [&] {
            auto found = references.getValue(pointer.offset);
            if(!found) return false;

            auto counts = found.unwrap();
            if(counts.other || counts.calls != 1) return false;
            if(callee->teardown || pointer == opt.program.entry) return false;

            return size <= limits.soleBudget;
        };

        /*
         * **A cold site is compiled the way `-inline size` compiles everything**, which is a
         * sentence the table already contains rather than a fourth number: `budgetFor(Size)` is
         * `{0, 96, 96, 0}` and its note says why - "only the case that cannot grow the program".
         * That is what an author writing `@cold` asked for, one path at a time.
         *
         * Two terms, and both of them are "this cannot grow the program". `kColdBudget` admits a
         * body no larger than the call it replaces; the sole reference admits a body that is being
         * moved rather than copied. What neither admits is the message an assertion's failure arm
         * builds - `String.ofCapacity`, a `push` per piece, a `show` per value, each with callers
         * elsewhere - and that is the whole of what the attribute was written for.
         *
         * The three terms it skips are the three that are reasons to *grow* the program: the flat
         * budget, the `@inline`/`@hot` hint and the chain. Each of them buys time, and time on a
         * path the author has said is rare is what there is nothing to trade for.
         */
        if(cold) return size <= kColdBudget || solelyReferenced();

        if(size <= limits.budget) return true;

        // A link of a chain, at `soleBudget` and for the same sentence the hint is weighed by: there
        // is a reason to copy this body that the body itself does not show.
        if(flattening && limits.budget && size <= limits.soleBudget) return true;

        /*
         * The bare `@inline` hint, weighed at `soleBudget` - one number, and it is the one already
         * in the table rather than a fourth.
         *
         * What the attribute says is that the author knows something the body does not show: a loop
         * this call sits in, or folding that only happens two passes later. What the *number* says
         * is how far that is worth trusting, and `soleBudget` is the honest answer here, because it
         * is already this file's statement of "a body this large is worth copying where there is a
         * reason to". A hint is a reason; being the only reference is the other one. Making it a
         * third budget between the two would be a number with nothing behind it.
         *
         * It cannot ask for something the pass would then quietly not do: the ceiling was compared
         * above, and every structural refusal - a closure, a lens, recursion, `@noinline` - is
         * ahead of this and reads no number at all.
         *
         * Declined at `Size`, which is what `limits.budget` being zero says: that level's whole
         * statement is that nothing grows the program, and a hint is a request to grow it. The two
         * sentences are one test because they are one number.
         */
        if(callee->inlineHint && limits.budget && size <= limits.soleBudget) return true;

        // `@hot`, at the same number and for the same sentence: it is a reason to copy a body that
        // the body itself does not show. A hint rather than a directive, so it reads the budget and
        // is declined at `Size` exactly as the line above it is.
        if(callee->hot && limits.budget && size <= limits.soleBudget) return true;

        return solelyReferenced();
    }


    // ---------------------------------------------------------------- the clone

    Value* mapValue(Clone& clone, SsaPtr<Value> value) {
        auto index = base()[value]->id.index;
        assertTrue(index < clone.values.size() && clone.values[index]);
        return base()[clone.values[index]];
    }

    /*
     * One operand mapped, and the one place a use kind is not carried across unchanged.
     *
     * A **borrowed parameter bound to an owned argument** is where the two conventions meet: inside
     * the callee the parameter is `Guaranteed`, so a use of it that owes nothing is an `Inspect`;
     * the caller's value is one the linear dataflow follows, and an `Inspect` of one of those is not
     * representable (`verify.cpp`'s rule, and it is the rule rather than a check - a tracked value's
     * uses *are* what the dataflow counts). A `Borrow` is the same use with the obligation restored,
     * which is what the call's own operand already was, so this restates the site rather than
     * inventing anything.
     *
     * The shape that reaches it is every adaptor chain: `call each borrow %xs borrow %pair` passes
     * an owned `{code, env}` to a `[shared call]` parameter, and the callee re-reads that parameter
     * at every `yield`.
     */
    Use mapUse(Clone& clone, Use used) {
        if(!used.isSet()) return Use();

        auto value = mapValue(clone, used.value());
        auto kind = used.kind();

        if(kind == UseKind::Inspect && isTracked(value->convention)) kind = UseKind::Borrow;

        return Use(value - base(), kind);
    }

    Block* mapBlock(Clone& clone, SsaPtr<Block> block) {
        auto index = base()[block]->index.index;
        assertTrue(index < clone.blocks.size() && clone.blocks[index]);
        return base()[clone.blocks[index]];
    }

    void mapUses(Clone& clone, UseList& from, SmallArray<Use, 8>& into) {
        for(auto used: from.contents(base())) into.push(mapUse(clone, used));
    }

    /*
     * One instruction re-created through the editor, at whatever insertion point the caller set.
     *
     * Through the editor rather than by copying the record, which is what keeps the use lists, the
     * results and the loan defaults right by construction: every `add*` records its operands and
     * gives its result the occurrence and convention it is handed. The result's occurrence and
     * convention are read off the *original's* result rather than re-derived, so nothing here has to
     * ask `conventionFor` a second time.
     */
    void cloneInst(Clone& clone, Editor& editor, Inst& from) {
        auto at = from.source;
        auto results = from.results.size();
        auto original = results ? base()[from.results.get(base(), 0)] : nullptr;

        auto occurrence = [&] { return original->occurrence; };
        auto convention = [&] { return original->convention; };

        Inst* made = nullptr;

        switch(from.kind) {
            case Inst::Const:
                made = editor.addConst(at, ((InstConst&)from).constant);
                break;

            case Inst::GlobalAddress:
                made = editor.addGlobalAddress(at, ((InstGlobalAddress&)from).global);
                break;

            /*
             * A run request, cloned with its placement bit **cleared** - which is what `addRunSlots`
             * gives it and is deliberate rather than an omission.
             *
             * `place.cpp` runs after inlining and answers the question for the frame the request
             * ends up in. A bit copied from the callee would be an answer about a different frame,
             * which is precisely the confusion the pass exists to avoid: `leaving() = [7, 8, 9]`
             * is the allocator's under its own name and the caller's frame once it is spliced in.
             */
            case Inst::RunSlots: {
                auto& slots = (InstRunSlots&)from;
                made = editor.addRunSlots(at, slots.element, slots.capacity);
                break;
            }

            case Inst::Metric: {
                auto& written = (InstMetric&)from;
                made = editor.addMetric(at, written.metric, written.of, occurrence().type);
                break;
            }

            case Inst::Native: {
                auto& written = (InstNative&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.arguments, arguments);

                made = editor.addNative(at, written.op, arguments.pointer(), arguments.size(),
                                        results ? occurrence() : Occurrence(),
                                        results ? convention() : Convention::Trivial, results != 0);
                break;
            }

            case Inst::Atomic: {
                auto& written = (InstAtomic&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.arguments, arguments);

                made = editor.addAtomic(at, written.op, written.order, written.failure, written.weak,
                                        arguments.pointer(), arguments.size(),
                                        results ? occurrence() : Occurrence(),
                                        results ? convention() : Convention::Trivial, results != 0);
                break;
            }

            case Inst::AtomicOk:
                made = editor.addAtomicOk(at, mapUse(clone, ((InstAtomicOk&)from).cas), occurrence());
                break;

            case Inst::Fma: {
                auto& written = (InstFma&)from;
                made = editor.addFma(at, mapUse(clone, written.lhs), mapUse(clone, written.rhs),
                                     mapUse(clone, written.addend), occurrence());
                break;
            }

            case Inst::Select: {
                auto& written = (InstSelect&)from;
                made = editor.addSelect(at, mapUse(clone, written.condition),
                                        mapUse(clone, written.whenTrue),
                                        mapUse(clone, written.whenFalse), occurrence(), convention());
                break;
            }

            /*
             * A `map`'s entries are copied entry for entry - the table is the instruction, so a
             * clone that shared the callee's list would be a body holding another body's region.
             */
            case Inst::Map: {
                auto& written = (InstMap&)from;

                SmallArray<mir::MapEntry, 16> entries;
                for(auto entry: written.entries.contents(base())) entries.push(entry);

                made = editor.addMap(at, mapUse(clone, written.selector), entries.pointer(),
                                     entries.size(), written.fallback, written.closed,
                                     occurrence(), convention());
                break;
            }

            case Inst::VecSplat:
                made = editor.addVecSplat(at, mapUse(clone, ((InstVecSplat&)from).from), occurrence());
                break;

            case Inst::VecLane: {
                auto& written = (InstVecLane&)from;
                made = editor.addVecLane(at, mapUse(clone, written.from), written.lane, occurrence());
                break;
            }

            case Inst::VecWithLane: {
                auto& written = (InstVecLane&)from;
                made = editor.addVecWithLane(at, mapUse(clone, written.from), written.lane,
                                             mapUse(clone, written.value), occurrence());
                break;
            }

            case Inst::VecShuffle: {
                auto& written = (InstVecShuffle&)from;
                SmallArray<U8, 16> pattern;
                for(auto lane: written.pattern.contents(base())) pattern.push(lane);

                made = editor.addVecShuffle(at, mapUse(clone, written.left),
                                            mapUse(clone, written.right), pattern.pointer(),
                                            pattern.size(), occurrence());
                break;
            }

            case Inst::VecReduce: {
                auto& written = (InstVecReduce&)from;
                made = editor.addVecReduce(at, mapUse(clone, written.from), written.reduce,
                                           occurrence());
                break;
            }

            case Inst::Alloc: {
                auto& written = (InstAlloc&)from;
                made = editor.addAlloc(at, written.held, written.storage);
                break;
            }

            case Inst::FieldAddress: {
                auto& written = (InstFieldAddress&)from;
                made = editor.addFieldAddress(at, mapUse(clone, written.base), written.record,
                                              written.field, written.shape);
                break;
            }

            case Inst::ElementAddress: {
                auto& written = (InstElementAddress&)from;
                made = editor.addElementAddress(at, mapUse(clone, written.base),
                                                mapUse(clone, written.index), written.container);
                break;
            }

            // The overread flag travels with the clone. A copy that dropped it is a load nothing
            // downstream knows is deliberately past the end - and `vectorPast` is only ever reached
            // through a library body, so *every* overreading load in a real program is an inlined
            // one.
            case Inst::Load:
                made = editor.addLoad(at, mapUse(clone, ((InstLoad&)from).address), occurrence(),
                                      convention(), ((InstLoad&)from).overread);
                break;

            case Inst::Copy:
                made = editor.addCopy(at, mapUse(clone, ((InstCopy&)from).address), occurrence(),
                                      convention());
                break;

            case Inst::Store: {
                auto& written = (InstStore&)from;
                made = editor.addStore(at, mapUse(clone, written.address),
                                       mapUse(clone, written.value));
                break;
            }

            case Inst::Binary: {
                auto& written = (InstBinary&)from;
                made = editor.addBinary(at, written.op, mapUse(clone, written.lhs),
                                        mapUse(clone, written.rhs), occurrence(), convention());
                break;
            }

            case Inst::Unary: {
                auto& written = (InstUnary&)from;
                made = editor.addUnary(at, written.op, mapUse(clone, written.value), occurrence(),
                                       convention());
                break;
            }

            case Inst::Aggregate: {
                auto& written = (InstAggregate&)from;
                SmallArray<Use, 8> components;
                mapUses(clone, written.components, components);

                made = editor.addAggregate(at, written.constructor, components.pointer(),
                                           components.size(), occurrence(), convention());
                break;
            }

            case Inst::Drop: {
                auto& written = (InstDrop&)from;
                made = editor.addDrop(at, mapUse(clone, written.address), written.held,
                                      written.boxesVisible);
                break;
            }

            /*
             * The release of an allocation the callee made - copied like any other effect.
             *
             * A body that allocates and frees in one activation carries both ends into its caller,
             * which is what makes inlining leave the pairing intact. What it does *not* do is
             * re-decide the placement: `escape-before-inlining` in the shipping stage is a demote
             * pass over the copied allocation, and there is nothing to demote here until the escape
             * half selects a rung on its own.
             */
            case Inst::Free:
                made = editor.addFree(at, mapUse(clone, ((InstFree&)from).address));
                break;

            case Inst::FunValue: {
                auto& written = (InstFunValue&)from;
                auto environment = mapUse(clone, written.environment);

                made = editor.addFunValue(at, written.callee,
                                          written.capturing ? &environment : nullptr, occurrence(),
                                          convention());
                break;
            }

            case Inst::Call: {
                auto& written = (InstCall&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.arguments, arguments);

                /*
                 * The call site rebuilt out of the fields it was built from. `environment` travels
                 * unchanged and needs no mapping: a `mir::GenEnvironment` is a declaration in the
                 * shared region, interned per (generic, argument list), so the witness table this
                 * call hands over is the same table wherever the call stands.
                 */
                mir::CallSite site;
                site.target = written.callee;
                site.through = written.through;
                site.requirement = written.requirement;
                site.member = written.member;
                site.environment = written.environment;

                made = editor.addCall(at, site, mapUse(clone, written.target), arguments.pointer(),
                                      arguments.size(), results ? occurrence() : Occurrence(),
                                      results ? convention() : Convention::Trivial, results != 0);
                break;
            }

            case Inst::GenValue:
            case Inst::Return:
            case Inst::Jump:
            case Inst::Branch:
            case Inst::Switch:
            case Inst::Unreachable:
            case Inst::KindCount:
                // The terminators are `cloneTerminator`'s, and the two refused kinds never reach a
                // body this pass admits - see `clonableBody`.
                assertTrue(false);
                return;
        }

        assertTrue(made != nullptr && made->results.size() == results);
        for(Size i = 0; i < results; i++) {
            clone.values[base()[from.results.get(base(), i)]->id.index] =
                editor.resultOf(*made, i) - base();
        }
    }

    /*
     * A returned value restated as the occurrence the call's result declares - the same handover
     * `Conversion::emitLeave` makes for the `@inline(always)` graft, and for the same reason.
     *
     * A `values`-shaped body returns the pointer it read out of a field, an owned `%a`, at a result
     * declared `%source'a`. A call boundary tolerates that silently because the callee's `return` is
     * not held to the declaration and the caller reads the declaration; a jump is held to its
     * target's argument and a substitution is held to its readers, so the restatement has to be
     * explicit. `represent` moves no bytes and the lowering passes the value through.
     */
    Value* restated(Editor& editor, Value& supplied, Value& declared, LocationId at) {
        if(supplied.occurrence == declared.occurrence) return &supplied;

        auto inst = editor.addUnary(at, mir::UnaryOp::Represent,
                                    editor.use(supplied, UseKind::Inspect), declared.occurrence,
                                    declared.convention);

        return editor.resultOf(*inst);
    }

    // Whether the two ends of the handover agree well enough to be joined - see the file comment's
    // last decline.
    bool handoverAgrees(Value* returned, Value* result) {
        if(!returned || !result) return returned == nullptr && result == nullptr;
        if(returned->convention != result->convention) return false;

        // A restatement is a `represent`, which is a pure instruction and may not consume - so a
        // tracked value that disagrees is a defect rather than something to coerce.
        return returned->occurrence == result->occurrence || !isTracked(returned->convention);
    }

    void cloneTerminator(Clone& clone, Editor& editor, Block& into, Inst& from) {
        auto at = from.source;

        switch(from.kind) {
            case Inst::Return: {
                auto& written = (InstReturn&)from;
                Value* returned = written.values.size()
                    ? mapValue(clone, written.values.get(base(), 0).value()) : nullptr;

                if(!clone.join) {
                    // The straight-line case has no edge: the value is handed to the splice, which
                    // points the call's readers at it.
                    clone.result = returned;
                    return;
                }

                if(!returned) {
                    editor.replaceTerminator(into, editor.makeJump(at, *clone.join, nullptr, 0));
                    return;
                }

                auto declared = base()[clone.join->argumentAt(base(), 0)];
                editor.setBlock(into);

                auto value = restated(editor, *returned, *declared, at);
                auto used = editor.use(*value, written.values.get(base(), 0).kind());
                editor.replaceTerminator(into, editor.makeJump(at, *clone.join, &used, 1));
                return;
            }

            case Inst::Jump: {
                auto& written = (InstJump&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.to.arguments, arguments);

                editor.replaceTerminator(into,
                    editor.makeJump(at, *mapBlock(clone, written.to.target), arguments.pointer(),
                                    arguments.size()));
                return;
            }

            case Inst::Branch: {
                // Each edge's own operands, which is `makeBranchEdges`' reason to exist: `promote`
                // places a block argument where one slot's writes meet, so the two arms of a branch
                // in a settled callee routinely declare different ones.
                auto& written = (InstBranch&)from;
                SmallArray<Use, 8> whenTrue;
                SmallArray<Use, 8> whenFalse;
                mapUses(clone, written.whenTrue.arguments, whenTrue);
                mapUses(clone, written.whenFalse.arguments, whenFalse);

                editor.replaceTerminator(into,
                    editor.makeBranchEdges(at, mapUse(clone, written.condition),
                                           *mapBlock(clone, written.whenTrue.target),
                                           whenTrue.pointer(), whenTrue.size(),
                                           *mapBlock(clone, written.whenFalse.target),
                                           whenFalse.pointer(), whenFalse.size()));
                return;
            }

            /*
             * A multi-way branch - the cases and one argument list, which `makeSwitch` gives to
             * every edge. The per-edge form `makeBranchEdges` exists for is not needed here: a
             * `switch` is built by the recognizer with one list and `promote` places a block
             * argument where slots meet, which is a *branch* shape.
             */
            case Inst::Switch: {
                auto& written = (InstSwitch&)from;
                auto items = written.cases.pointer(base());
                auto count = written.cases.size();

                SmallArray<I64, 16> values;
                SmallArray<Block*, 16> targets;

                for(Size i = 0; i < count; i++) {
                    values.push(items[i].value);
                    targets.push(mapBlock(clone, items[i].edge.target));
                }

                SmallArray<Use, 8> arguments;
                mapUses(clone, written.fallback.arguments, arguments);

                editor.replaceTerminator(into,
                    editor.makeSwitch(at, mapUse(clone, written.selector), values.pointer(),
                                      targets.pointer(), count,
                                      *mapBlock(clone, written.fallback.target),
                                      arguments.pointer(), arguments.size()));
                return;
            }

            default:
                editor.replaceTerminator(into, editor.makeUnreachable(at));
                return;
        }
    }

    /*
     * The loans, re-minted in the caller's table once every value exists - the file comment's third
     * mapping.
     *
     * After the instructions rather than before them, because a loan names *storage values* and a
     * value names its loan: minting first would need the clones of the allocations, and setting the
     * dependency first would need the loans. The two halves are separate operations on the editor
     * for exactly this reason - `Editor::addResult` gives every borrow the caller's loan and
     * `setDependency` refines it - so the order falls out.
     */
    // One lender into a set, once. A linear scan: these sets are two or three entries.
    static void pushLender(SmallArray<SsaPtr<Value>, 4>& into, SsaPtr<Value> held) {
        for(auto already: into) {
            if(already == held) return;
        }

        into.push(held);
    }

    /*
     * What one lender the **callee** named becomes in the caller - the file comment's fourth
     * mapping, and the whole of what re-minting a loan is.
     *
     * Three answers, in the order of how much is known:
     *
     *  - **storage this clone made.** An allocation of the callee's frame becomes an allocation of
     *    the caller's, and the loan over it ends where the clone of its `drop` does. Exact.
     *  - **a parameter**, whose lender is the caller's: the *argument*'s own loan already names
     *    which storage that is, so the answer is its members. Exact too, and it is what makes
     *    `fn middle(a: x'G, b: y'G, c: z'G) -> y'G` come back rooted in the second argument's
     *    lender rather than in all three - a union there would be a loan the caller's own call
     *    result is not on, which `Editor::replaceAllUses` refuses outright.
     *  - **a borrow of the callee's**, whose lender is whatever *it* was rooted in - the same
     *    question one level down. At the end of that chain is either a parameter or the callee's
     *    unrooted loan, and the unrooted one is `Clone::outside`: the callee could only say
     *    "somewhere outside", and the caller's borrowing arguments are where that is.
     *
     * Bounded by depth rather than by a visited set, because a loan rooted in a block argument can
     * name a value rooted in it again round a back edge, and eight levels of reborrowing is past
     * anything a body writes. Falling out of it lands on `outside`, which is the same over-report
     * the third case is.
     *
     * Taking every one of these to the *empty* loan - which is what this did - hands the caller a
     * borrow rooted nowhere where the call's own result was rooted in the caller's slot, so every
     * use of it is outside what `verifyBody`'s third rule can measure. A callee whose result borrows
     * a field of a borrow it took through a pointer (`lib/Core/Map`'s three accessors, and
     * `elementField` in `loans/PointeeOrigin.yana`) went further and minted a body-local loan over
     * the cloned borrow, which is neither the caller's loan nor the empty one.
     */
    void rootLender(Clone& clone, SsaPtr<Value> held, SmallArray<SsaPtr<Value>, 4>& into,
                    U32 depth) {
        static constexpr U32 kMaxReborrowDepth = 8;

        auto index = base()[held]->id.index;

        if(index < clone.values.size() && clone.values[index]) {
            if(clone.parameters[index]) {
                auto& argument = *base()[clone.values[index]];

                if(argument.convention == Convention::Guaranteed && argument.loan) {
                    auto& lent = *base()[argument.loan];
                    if(lent.storage.size()) {
                        for(auto member: lent.storage.contents(base())) pushLender(into, member);
                        return;
                    }
                }
            } else {
                auto& source = *clone.source.value(base(), ValueId(index));

                if(source.convention == Convention::Address) {
                    pushLender(into, clone.values[index]);
                    return;
                }

                if(depth < kMaxReborrowDepth && source.convention == Convention::Guaranteed &&
                   source.loan) {
                    auto& inner = *base()[source.loan];
                    if(inner.storage.size()) {
                        for(auto member: inner.storage.contents(base())) {
                            rootLender(clone, member, into, depth + 1);
                        }

                        return;
                    }
                }
            }
        }

        for(auto lender: clone.outside) pushLender(into, lender);
    }

    void mintLoans(Clone& clone, Editor& editor) {
        clone.loans.reserve(clone.source.loans.size());

        for(U32 i = 0; i < clone.source.loans.size(); i++) {
            auto loan = clone.source.loan(base(), LoanId(i));

            SmallArray<SsaPtr<Value>, 4> storage;

            for(auto held: loan->storage.contents(base())) rootLender(clone, held, storage, 0);

            clone.loans.push(editor.loanOn(loan->source, storage.pointer(), storage.size(),
                                           loan->capability) - base());
        }

        for(U32 i = 0; i < clone.source.values.size(); i++) {
            if(i >= clone.values.size() || !clone.values[i] || clone.parameters[i]) continue;

            auto value = clone.source.value(base(), ValueId(i));
            if(value->convention != Convention::Guaranteed || !value->loan) continue;

            auto source = base()[value->loan]->id.index;
            if(source >= clone.loans.size()) continue;

            editor.setDependency(*base()[clone.values[i]], *base()[clone.loans[source]]);
        }
    }

    // ---------------------------------------------------------------- the splice

    /*
     * The parameters bound to the call's operands, which is the whole of what a callee's entry block
     * becomes - `resolve_ssa/README.md`'s "parameters arrive as entry-block arguments" read from the
     * caller's side.
     *
     * `parameters` is the other half and is what `mintLoans` reads: these positions map to values the
     * *caller* owns, so nothing about their loans is this clone's to decide.
     */
    void bindParameters(Clone& clone, Block& entry, InstCall& call) {
        auto count = clone.source.values.size();

        clone.values.reserve(count);
        clone.parameters.reserve(count);

        for(U32 i = 0; i < count; i++) {
            clone.values.push(nullptr);
            clone.parameters.push(false);
        }

        clone.blocks.reserve(clone.source.blocks.size());
        for(U32 i = 0; i < clone.source.blocks.size(); i++) clone.blocks.push(nullptr);

        for(Size i = 0; i < entry.argumentCount(); i++) {
            auto index = base()[entry.argumentAt(base(), i)]->id.index;
            clone.values[index] = call.arguments.get(base(), i).value();
            clone.parameters[index] = true;
        }

        // And what the callee could only call "outside" - see `Clone::outside`. Over every argument
        // rather than over the parameters alone, because a witness table or an environment operand
        // is an argument the callee's entry has no block argument for and it lends storage all the
        // same.
        for(auto use: call.arguments.contents(base())) {
            if(use.kind() == UseKind::Consume) continue;

            auto& value = *base()[use.value()];
            if(value.convention != Convention::Guaranteed || !value.loan) continue;

            for(auto held: base()[value.loan]->storage.contents(base())) addOutside(clone, held);
        }
    }

    // One lender into the may-set, once. A linear scan because the set is the arity of the call and
    // an argument list that would make this cost anything is one the budget refused long before.
    static void addOutside(Clone& clone, SsaPtr<Value> held) {
        for(auto already: clone.outside) {
            if(already == held) return;
        }

        clone.outside.push(held);
    }

    /*
     * A callee of one block ending in `return`, spliced into the caller's own block.
     *
     * The whole of it is a value map: the instructions are re-created in front of the call, the
     * `return` hands back a value, and every reader of the call's result is pointed at it. No block
     * is made and no edge moves, which is what makes this the shape that keeps the dump readable and
     * the block-local passes below still able to see the callee's storage.
     */
    void spliceStraightLine(Editor& editor, mir::Function& callee, InstCall& call, Block& block) {
        auto body = base()[callee.ssa];
        auto entry = base()[body->entry];

        Clone clone { *body };
        bindParameters(clone, *entry, call);
        clone.blocks[entry->index.index] = &block - base();

        editor.setInsertBefore(call);

        for(auto pointer: entry->instructions(base())) {
            auto inst = base()[pointer];
            if(isTerminator(inst->kind)) break;

            cloneInst(clone, editor, *inst);
        }

        // Which sets `clone.result` rather than editing anything: with no join there is no edge for
        // a `return` to become.
        cloneTerminator(clone, editor, block, *entry->terminator(base()));
        mintLoans(clone, editor);

        if(call.results.size()) {
            auto result = base()[call.results.get(base(), 0)];
            editor.setInsertBefore(call);
            editor.replaceAllUses(*result, *restated(editor, *clone.result, *result, call.source));
        }

        editor.eraseInst(call);
    }

    /*
     * The general form: the caller's block cut in two, the callee's blocks in the gap, and a join
     * carrying the result.
     *
     * The join is a block of its own rather than the second half of the split - see the file
     * comment. What makes that cheap is that `cleanCfg` runs immediately below this in the driver
     * and folds the pair, since the join's one successor has exactly one predecessor.
     */
    void spliceControlFlow(Editor& editor, mir::Function& callee, InstCall& call, Block& block,
                           Size position) {
        auto body = base()[callee.ssa];
        auto entry = base()[body->entry];
        auto result = call.results.size() ? base()[call.results.get(base(), 0)] : nullptr;

        Clone clone { *body };
        reversePostorder(base(), *body, clone.order);
        bindParameters(clone, *entry, call);

        auto continuation = editor.splitBlock(block, position + 1);

        // Every block this makes is in the call site's region, which the attaching editor does not
        // start in: its cursor is the entry block's.
        editor.setRegion(block.region);

        auto join = editor.addBlock(block.name);
        clone.join = join;

        if(result) editor.addBlockArgument(*join, result->occurrence, result->convention);
        editor.replaceTerminator(*join, editor.makeJump(call.source, *continuation, nullptr, 0));

        /*
         * The regions, before any block is made: a block names the region it is in when it is
         * created, and `addBlock` claims the first one made in a region as its `entry`.
         *
         * The callee's root maps to the region the call site is in. Every other one is re-created
         * under its own mapped parent, which is why the walk is in id order - `enterRegion` takes
         * the parent from the cursor and a region is opened inside one that already exists, so a
         * parent's id is always below its child's.
         */
        clone.regions.reserve(body->regions.size());
        for(U32 i = 0; i < body->regions.size(); i++) clone.regions.push(ContinuationId());

        for(U32 i = 0; i < body->regions.size(); i++) {
            auto region = body->region(base(), ContinuationId(i));

            if(!region->parent.valid()) {
                clone.regions[i] = block.region;
                continue;
            }

            editor.setRegion(clone.regions[region->parent.index]);
            clone.regions[i] = editor.enterRegion(region->kind, region->multiplicity);
            editor.leaveRegion();
        }

        for(auto index: clone.order) {
            auto from = body->block(base(), BlockId(index));
            editor.setRegion(clone.regions[from->region.index]);
            clone.blocks[index] = editor.addBlock(from->name) - base();
        }

        editor.setRegion(block.region);

        // The destinations, once every block exists - `rejoinRegion` names a block outside the
        // region it belongs to, so both have to be mapped first.
        for(U32 i = 0; i < body->regions.size(); i++) {
            auto region = body->region(base(), ContinuationId(i));
            if(!region->parent.valid() || !region->destination.valid()) continue;

            auto destination = clone.blocks[region->destination.index];
            if(destination) editor.rejoinRegion(clone.regions[i], *base()[destination]);
        }

        // Every block's arguments before any instruction anywhere, which is `addBlockArgument`'s own
        // rule: a parameter holds on entry, so a block that has been filled refuses one.
        for(auto index: clone.order) {
            if(BlockId(index) == entry->index) continue;

            auto from = body->block(base(), BlockId(index));
            auto target = base()[clone.blocks[index]];

            for(auto pointer: from->arguments(base())) {
                auto argument = base()[pointer];
                clone.values[argument->id.index] =
                    editor.addBlockArgument(*target, argument->occurrence,
                                            argument->convention) - base();
            }
        }

        for(auto index: clone.order) {
            auto from = body->block(base(), BlockId(index));
            editor.setBlock(*base()[clone.blocks[index]]);

            for(auto pointer: from->instructions(base())) {
                auto inst = base()[pointer];
                if(isTerminator(inst->kind)) break;

                cloneInst(clone, editor, *inst);
            }
        }

        // The terminators last, because one names blocks and block arguments the loop above is what
        // makes exist.
        for(auto index: clone.order) {
            auto from = body->block(base(), BlockId(index));
            cloneTerminator(clone, editor, *base()[clone.blocks[index]], *from->terminator(base()));
        }

        mintLoans(clone, editor);

        // And the way in, which is the last edit that touches the caller's own block: the call is
        // still standing in it and is erased once nothing reads its result.
        editor.replaceTerminator(block, editor.makeJump(call.source,
                                                        *base()[clone.blocks[entry->index.index]],
                                                        nullptr, 0));

        if(result) editor.replaceAllUses(*result, *base()[join->argumentAt(base(), 0)]);
        editor.eraseInst(call);
    }

    Size reachableBlockCount(Body& body) {
        Array<U32> order;
        reversePostorder(base(), body, order);
        return order.size();
    }

    /*
     * Whether every `return` in the callee hands over what the call's result declares - asked before
     * anything is edited, because a splice half made cannot be taken back.
     *
     * And whether there is one at all, which is the other half of the same question. A callee that
     * cannot return - one settled down to an endless loop, whose exit test the folder decided and
     * `cleanCfg` then deleted the arm of - reaches the general splice with **no** cloned `return` to
     * jump to the join. The join is then a block nothing enters, holding the block argument every
     * reader of the call's result has just been pointed at, and the continuation reads a value no
     * path defines: `verifyBody` reports it at this pass's own checkpoint. What a splice would buy
     * here is the caller's tail becoming unreachable, which is the correct answer and is a different
     * rewrite - deleting the continuation, not filling it - so the call is declined instead.
     */
    bool handoverChecks(Body& body, Value* result) {
        Array<U32> order;
        reversePostorder(base(), body, order);

        auto returns = false;

        for(auto index: order) {
            auto terminator = body.block(base(), BlockId(index))->terminator(base());
            if(!terminator || terminator->kind != Inst::Return) continue;

            returns = true;

            auto& written = (InstReturn&)*terminator;
            if(written.values.size() != Size(result ? 1 : 0)) return false;
            if(!result) continue;

            if(!handoverAgrees(base()[written.values.get(base(), 0).value()], result)) return false;
        }

        return returns;
    }

    // ---------------------------------------------------------------- adaptor chains

    /*
     * The `fun.value` a call goes through, where **this body** is what built it.
     *
     * Four steps, and each of them is a restatement of one value rather than a guess about which of
     * several it might be:
     *
     *  - a **`represent`**, which is what "the same bytes, a different statement" is spelled as. A
     *    borrowed function value, a pointer to the pair and the pair itself are one word with three
     *    readings, and the conversion and this pass both emit the restatement between them.
     *  - a **`load`**, which at this tier is either a read *through* a borrow of the pair - the
     *    conversion emits one at every `yield` - or a read of the slot below. Both are answered by
     *    walking to the address, so there is no case here.
     *  - a **frame slot with one writer**: an `alloc` whose users are that one `store` and loads.
     *    Any load that runs reads what that store wrote, because a load before it would be reading
     *    storage nothing filled, which MIR has already refused. `let f = (v) -> v * scale` puts the
     *    pair in exactly such a slot, and without this step every lambda a chain is written with
     *    stays an indirect call.
     *  - a **block argument** stops it, and that is decided rather than missing: two edges carry two
     *    values, so a `fun.value` on one of them is not what the call goes through.
     */
    InstFunValue* funValueBehind(Value& target) {
        static constexpr U32 kMaxSteps = 12;

        auto value = &target;

        for(U32 step = 0; step < kMaxSteps; step++) {
            if(!value->definer) return nullptr;

            auto inst = base()[value->definer];

            switch(inst->kind) {
                case Inst::FunValue:
                    return (InstFunValue*)inst;

                case Inst::Unary:
                    if(((InstUnary&)*inst).op != mir::UnaryOp::Represent) return nullptr;
                    value = base()[((InstUnary&)*inst).value.value()];
                    continue;

                case Inst::Load:
                    value = base()[((InstLoad&)*inst).address.value()];
                    continue;

                case Inst::Alloc: {
                    auto stored = soleStoredValue(*value);
                    if(!stored) return nullptr;

                    value = stored;
                    continue;
                }

                default:
                    return nullptr;
            }
        }

        return nullptr;
    }

    /*
     * The one value a frame slot is ever given - the third step above.
     *
     * Every user is a `load` or the one `store`, so nothing writes the slot twice and nothing takes
     * its address anywhere this cannot see: a `field.addr` of it, a call handed it or a `drop` of it
     * all leave, because each is a route by which the bytes could become something else.
     */
    Value* soleStoredValue(Value& slot) {
        Value* stored = nullptr;

        for(auto pointer: slot.uses(base())) {
            auto user = base()[pointer];

            if(user->kind == Inst::Load) continue;
            if(user->kind != Inst::Store) return nullptr;

            auto& written = (InstStore&)*user;
            if(written.address.value() != &slot - base()) return nullptr;
            if(stored) return nullptr;

            stored = base()[written.value.value()];
        }

        return stored;
    }

    /*
     * A call through a function value this body built, rewritten as a call to the declaration that
     * value names - the devirtualization half of flattening a chain.
     *
     * The environment becomes argument zero, which is not a convention this pass invents: a function
     * value is `{code, env}` and `code` always takes the environment first
     * ([[yana-function-values]]), so a capturing value's declaration already *has* that parameter
     * and a non-capturing one reaches its declaration through a thunk that only absorbs it. Removing
     * the thunk is what the second case buys; the first is what makes the continuation inlinable at
     * all, because a `CallThrough::Value` site names no body.
     *
     * Worth making on its own terms even where the inliner then declines the body: an indirect call
     * through two loads becomes a direct one, and the callee's summary becomes readable at the site.
     */
    bool devirtualizeSite(InstCall& call) {
        if(call.through != mir::CallThrough::Value || !call.target.isSet()) return false;

        auto made = funValueBehind(*base()[call.target.value()]);
        if(!made || !made->callee) return false;

        auto callee = decl()[made->callee];

        /*
         * A generic compiled once is reached across the erased ABI, whose argument zero is a witness
         * table rather than an environment - so a value of one is a *thunk* over the pair and the
         * table it closes over is not written down here. `InstCall::environment` is what a direct
         * call to one would need and a `fun.value` does not carry it.
         */
        if(mir::isGeneric(decl(), callee->gen)) return false;

        auto written = call.arguments.size();
        if(callee->args.size() != written + Size(made->capturing ? 1 : 0)) return false;

        /*
         * The operands restated as the **declaration** asks for them, which is the one thing a call
         * through a value and a direct call to the same body disagree about.
         *
         * A function type's positions carry contracts of their own, so the pair's call site supplies
         * an `Inspect` wherever nothing is owed - and a direct call is held to `argumentMode`, which
         * answers `Consume` or `Borrow` and never `Inspect` (`verify.cpp` states the rule, and this
         * reads it rather than copying it). Weakening a borrow into an inspection is what the two
         * differ by and restoring it is free; the two directions that are *not* free are refused,
         * because each would move an obligation: a consumed operand at a borrowed position leaves an
         * owner unconsumed below the call, and a borrowed one at a consuming position hands over
         * storage this frame still names. Neither is reachable through a `fun.value` today - a
         * continuation's environment is declared `borrowed` - so this is the rule rather than a
         * workaround for a shape in the corpus.
         */
        SmallArray<Use, 8> arguments;

        auto declared = [&](Size index, Use used, Use& into) {
            auto& parameter = *decl()[callee->args.get(decl(), index)];
            auto mode = mir::argumentMode(parameter.occurrence.contract, mir::SourceKind::Result);
            auto wanted = mode == mir::UseMode::Take ? UseKind::Consume : UseKind::Borrow;

            if((used.kind() == UseKind::Consume) != (wanted == UseKind::Consume)) return false;

            into = used.withKind(wanted);
            return true;
        };

        Size position = 0;

        if(made->capturing) {
            Use environment;
            if(!declared(position++, made->environment, environment)) return false;

            arguments.push(environment);
        }

        for(auto used: call.arguments.contents(base())) {
            Use supplied;
            if(!declared(position++, used, supplied)) return false;

            arguments.push(supplied);
        }

        auto result = call.results.size() ? base()[call.results.get(base(), 0)] : nullptr;

        mir::CallSite site;
        site.target = made->callee;

        auto editor = opt.ir();
        editor.setInsertBefore(call);

        auto direct = editor.addCall(call.source, site, Use(), arguments.pointer(),
                                     arguments.size(), result ? result->occurrence : Occurrence(),
                                     result ? result->convention : Convention::Trivial,
                                     result != nullptr);

        if(result) {
            auto answered = editor.resultOf(*direct);

            // The site's own loan, carried over rather than re-derived: the two calls are one
            // relationship and the readers below have already been measured against this one.
            if(result->loan) editor.setDependency(*answered, *base()[result->loan]);

            editor.replaceAllUses(*result, *answered);
        }

        editor.eraseInst(call);
        return true;
    }

    // Whether a site hands over a function value this body built - the consumer's end of a chain,
    // and where flattening starts. `each(xs, {code, env})` is the shape.
    bool handsOverLocalFunValue(InstCall& call) {
        for(auto used: call.arguments.contents(base())) {
            if(funValueBehind(*base()[used.value()])) return true;
        }

        return false;
    }

    /*
     * An adaptor chain collapsed into the loop that consumes it - the devirtualize-then-inline pair
     * run to a fixed point over one body.
     *
     * `for x in mapped(xs, f)` converts to four things in the consumer (an environment record, the
     * `{code, env}` pair over the continuation the region was outlined into, the call, and a test of
     * the exit code) and one indirect call *per element* inside the callee. Neither half is
     * removable on its own: the indirect call is in a frame that only has the pair as a parameter,
     * and the pair is in a frame that never calls it. So the two steps alternate - copy the
     * iterator's body in, which brings the `call [value]` into the frame that built the value;
     * devirtualize it; copy the continuation in, which brings the *next* link's `call [value]` in
     * with it - and a three-stage chain is six of those.
     *
     * **Two budgets bound it and neither is the per-callee one.** `kMaxChainDepth` is how many links
     * one body may absorb, and `limits.chain` is how much they may weigh together. The per-callee
     * number still applies underneath - `worthInlining`'s `flattening` term weighs each link at
     * `soleBudget` - so what these two add is the statement the per-callee number cannot make: a
     * chain is a *product* rather than a sum, since every link copied brings its own sites with it.
     * A three-stage chain flattens and a long one stops, which is the shape
     * [[yana-adaptor-chain-budget]] measured over there.
     *
     * The recursion guard is underneath both and is not relaxed: `clonableDeclaration` refuses any
     * member of a call-graph cycle, so a self-recursive iterator is left as a call and
     * [[yana-inliner-lens-recursion-runaway]] has nothing here to run away through.
     */
    bool flattenContinuations(mir::Function& function) {
        auto flattened = false;
        U32 spent = 0;

        Array<SsaPtr<Inst>> sites;

        for(U32 depth = 0; depth < kMaxChainDepth; depth++) {
            sites.clear();

            for(U32 i = 0; i < opt.body->blocks.size(); i++) {
                auto block = opt.body->block(base(), BlockId(i));
                if(block->deleted) continue;

                for(auto pointer: block->instructions(base())) {
                    if(base()[pointer]->kind == Inst::Call) sites.push(pointer);
                }
            }

            auto moved = false;

            for(auto pointer: sites) {
                auto inst = base()[pointer];
                if(!inst->block) continue;

                auto& call = (InstCall&)*inst;

                if(call.through == mir::CallThrough::Value) {
                    moved = devirtualizeSite(call) || moved;
                    continue;
                }

                if(call.through != mir::CallThrough::Declaration || !call.callee) continue;
                if(spent >= limits.chain) continue;

                // The two ends of a link: the continuation a devirtualized call now names, and the
                // consumer's own call into the iterator that will call it.
                auto callee = decl()[call.callee];
                if(!callee->environment && !handsOverLocalFunValue(call)) continue;

                if(!inlineSite(function, call, true)) continue;

                spent += sizeOf(*callee);
                moved = true;
            }

            if(!moved) break;
            flattened = true;
        }

        return flattened;
    }

    bool inlineSite(mir::Function& caller, InstCall& call, bool flattening = false) {
        if(call.through != mir::CallThrough::Declaration || !call.callee) return false;
        if(!clonableDeclaration(caller, call.callee)) return false;

        /*
         * The callee settled *before* anything about its body is read, which is not only the size
         * question: the rounds rewrite the body this is about to copy, so a refusal or a handover
         * checked against the unsettled form would be a statement about IR that no longer exists.
         */
        settleCallee(call.callee);

        auto callee = decl()[call.callee];
        if(!clonableBody(*callee)) return false;

        auto body = base()[callee->ssa];
        auto entry = base()[body->entry];

        // A signature the site does not fill is a defect rather than a case: the call was built from
        // this declaration's argument list and the entry's arguments are that list.
        assertTrue(entry->argumentCount() == call.arguments.size());
        if(entry->argumentCount() != call.arguments.size()) return false;

        auto result = call.results.size() ? base()[call.results.get(base(), 0)] : nullptr;
        if(!handoverChecks(*body, result)) return false;

        // Whether the *caller's* block is rare - see `findColdBlocks`. A fact about the site rather
        // than about the callee, which is why it is read here and passed in.
        if(!worthInlining(call.callee, flattening, inColdBlock(call.block))) return false;

        auto block = base()[call.block];
        Size position = 0;
        auto found = false;

        for(Size i = 0; i < block->instructionCount(); i++) {
            if(base()[block->instructionAt(base(), i)] != &call) continue;

            position = i;
            found = true;
            break;
        }

        if(!found) return false;

        auto editor = opt.ir();

        if(entry->terminator(base())->kind == Inst::Return && reachableBlockCount(*body) == 1) {
            spliceStraightLine(editor, *callee, call, *block);
        } else {
            spliceControlFlow(editor, *callee, call, *block, position);
        }

        return true;
    }

    // ---------------------------------------------------------------- the driver

    /*
     * How many times each declaration is named, over the bodies the program can still reach.
     *
     * Reachable only, and that is the decided half rather than an economy:
     * [[yana-escape-before-inlining]] item 5 is a call site in a function nothing could run being
     * counted, which turned a callee's one site into two and refused it. `reach` runs immediately in
     * front of this for exactly that reason.
     *
     * The same rule applies one level down, at a block rather than a whole function - and here the
     * walk has to say so itself, because `deleted` alone does not: `endNonReturningBlocks` runs
     * standalone in front of the inliner, cutting the return edge out of a diverging call's block
     * without a `cleanCfg` behind it to sweep the block that edge stranded. `reversePostorder` is
     * `sizeOf`'s answer to the identical question below - one walk of what a body can still run,
     * shared rather than restated with a weaker test.
     */
    void countReferences() {
        references.clear();

        auto count = [&](DeclPtr<mir::Function> callee, bool isCall) {
            if(!callee) return;

            auto entry = references.add(callee.offset);
            if(!entry.existed) *entry.value = References();

            if(isCall) entry.value->calls++; else entry.value->other++;
        };

        Array<U32> order;

        for(auto modulePointer: opt.program.modules) {
            auto module = decl()[modulePointer];

            for(auto pointer: module->functions.contents(decl())) {
                auto function = decl()[pointer];
                if(!function->reachable || !function->ssa) continue;

                auto body = base()[function->ssa];
                reversePostorder(base(), *body, order);

                for(auto index: order) {
                    auto block = body->block(base(), BlockId(index));

                    for(auto instPointer: block->instructions(base())) {
                        auto inst = base()[instPointer];

                        if(inst->kind == Inst::Call) {
                            auto& call = (InstCall&)*inst;
                            count(call.callee, call.through == mir::CallThrough::Declaration);

                            if(call.environment) {
                                for(auto witness: decl()[call.environment]->slots.contents(decl())) {
                                    for(auto member: witness.functions.contents(decl())) {
                                        count(member, false);
                                    }
                                }
                            }

                            continue;
                        }

                        if(inst->kind == Inst::FunValue) count(((InstFunValue&)*inst).callee, false);
                    }
                }
            }
        }
    }

    bool runFunction(DeclPtr<mir::Function> pointer) {
        auto& function = *decl()[pointer];

        opt.function = &function;
        opt.body = base()[function.ssa];

        findColdBlocks(*opt.body);

        /*
         * Every call in the body, read out before any of them is spliced.
         *
         * A splice moves instructions between blocks and erases the call, so a walk holding a block
         * cursor is a walk over a list that has changed underneath it. What this list can hold that
         * is no longer there is exactly one thing - a call that was erased - and `Inst::block` says
         * so, because `eraseInst` unlinks it. A call the clone *brought* is not in the list and is
         * the next round's.
         */
        pending.clear();

        for(U32 i = 0; i < opt.body->blocks.size(); i++) {
            auto block = opt.body->block(base(), BlockId(i));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(base())) {
                if(base()[pointer]->kind == Inst::Call) pending.push(pointer);
            }
        }

        auto inlined = false;

        for(auto pointer: pending) {
            auto inst = base()[pointer];
            if(!inst->block) continue;

            inlined = inlineSite(function, (InstCall&)*inst) || inlined;
        }

        /*
         * And then the chain, after the ordinary sites rather than among them: what makes a link
         * flattenable is that the link above it has already been copied in, so a pass that weighed
         * the two together would be deciding the second before the shape that justifies it exists.
         */
        inlined = flattenContinuations(function) || inlined;

        if(!inlined) return false;

        checkpoint(opt, "after inlineCalls"_v);

        /*
         * And the caller folded back to a fixed point, which is the other half of
         * [[yana-inline-size-honesty]]: the next site weighed against *this* body has to see what a
         * backend would emit rather than what a splice left behind. `settled` is cleared for it so
         * that a later caller re-settles rather than reading the memo taken before the splice.
         */
        settled.remove(pointer.offset);
        settle(function);
        return true;
    }
};

/*
 * Which functions can reach themselves through a chain of calls, found once for the whole program -
 * Tarjan's algorithm, because a strongly connected component *is* a cycle and the one-member case a
 * self-edge makes is the one a component numbering cannot tell from an ordinary function.
 *
 * Computed before the rounds and not between them, which is sound rather than a shortcut: a clone
 * gives its caller edges only to what it could already reach through the call that went away, so no
 * cycle is created and none is destroyed.
 *
 * Every kind of call is an edge here, not only the ones this pass would clone. What the graph is for
 * is deciding whether a *body* is recursive, and a body that reaches itself through a class dispatch
 * is as recursive as one that does not.
 */
void findRecursion(OptContext& opt, HashMap<U32, bool>& recursive) {
    Array<DeclPtr<mir::Function>> nodes;
    HashMap<U32, U32> index;

    Size functionCount = 0;
    for(auto modulePointer: opt.program.modules) {
        functionCount += opt.decl[modulePointer]->functions.size();
    }

    index.reserve(functionCount);
    recursive.reserve(functionCount);
    nodes.reserve(U32(functionCount));

    for(auto modulePointer: opt.program.modules) {
        for(auto pointer: opt.decl[modulePointer]->functions.contents(opt.decl)) {
            auto entry = index.add(pointer.offset);
            if(entry.existed) continue;

            *entry.value = U32(nodes.size());
            nodes.push(pointer);
        }
    }

    // The call graph as one array of targets with a start offset per node, which is two allocations
    // for the graph rather than one per function in the program.
    Array<U32> edgeData;
    Array<U32> edgeStart;

    for(Size i = 0; i < nodes.size(); i++) {
        edgeStart.push(U32(edgeData.size()));

        auto function = opt.decl[nodes[i]];
        if(!function->ssa) continue;

        auto body = opt.base[function->ssa];

        for(U32 b = 0; b < body->blocks.size(); b++) {
            auto block = body->block(opt.base, BlockId(b));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(opt.base)) {
                auto inst = opt.base[pointer];

                DeclPtr<mir::Function> callee = nullptr;
                if(inst->kind == Inst::Call) callee = ((InstCall&)*inst).callee;
                else if(inst->kind == Inst::FunValue) callee = ((InstFunValue&)*inst).callee;

                if(!callee) continue;

                auto found = index.getValue(callee.offset);
                if(found) edgeData.push(found.unwrap());
            }
        }
    }

    edgeStart.push(U32(edgeData.size()));

    auto edgeCount = [&](Size node) { return Size(edgeStart[node + 1] - edgeStart[node]); };
    auto edgeAt = [&](Size node, Size which) { return edgeData[edgeStart[node] + which]; };

    auto callsItself = [&](Size node) {
        for(Size e = 0; e < edgeCount(node); e++) {
            if(edgeAt(node, e) == U32(node)) return true;
        }

        return false;
    };

    auto count = nodes.size();
    Array<U32> number, lowlink;
    ScratchSet onStack(opt.sets, count);

    for(Size i = 0; i < count; i++) {
        number.push(0);
        lowlink.push(0);
    }

    // Explicit stacks rather than recursion: a call chain in a large program is deeper than this
    // process's own stack is willing to be.
    Array<U32> component;
    Array<U32> pending;
    Array<Size> next;
    Array<U32> members;
    U32 counter = 1;

    for(Size root = 0; root < count; root++) {
        if(number[root]) continue;

        number[root] = lowlink[root] = counter++;
        onStack->set(root, true);
        component.push(U32(root));
        pending.push(U32(root));
        next.push(0);

        while(pending.size()) {
            auto node = pending[pending.size() - 1];

            if(next[next.size() - 1] < edgeCount(node)) {
                auto target = edgeAt(node, next[next.size() - 1]++);

                if(!number[target]) {
                    number[target] = lowlink[target] = counter++;
                    onStack->set(target, true);
                    component.push(target);
                    pending.push(target);
                    next.push(0);
                } else if((*onStack)[target] && number[target] < lowlink[node]) {
                    lowlink[node] = number[target];
                }

                continue;
            }

            pending.pop();
            next.pop();

            if(lowlink[node] == number[node]) {
                members.clear();
                while(component.size()) {
                    auto member = component.pop().unwrap();
                    onStack->set(member, false);
                    members.push(member);

                    if(member == node) break;
                }

                // A one-member component is a cycle only through an edge to itself, which is the
                // case the component numbering cannot report.
                if(members.size() == 1 && !callsItself(node)) continue;

                for(auto member: members) recursive.add(nodes[member].offset, true);
            }

            if(pending.size() && lowlink[node] < lowlink[pending[pending.size() - 1]]) {
                lowlink[pending[pending.size() - 1]] = lowlink[node];
            }
        }
    }
}

} // namespace

void inlineCalls(OptContext& opt) {
    Inliner inliner { opt, budgetFor(opt.context.settings.inlining) };

    /*
     * **Not** an early-out at `InlineLevel::None` - J49.
     *
     * It was one, on the reading that a zero ceiling leaves nothing qualifying. That is true of
     * everything weighed and false of the one thing that is not: `@inline(always)` is a directive,
     * the conversion's graft applies no size test to it, and a call the graft declined - eight
     * expansions deep, or through a value - has only this pass left to honour it. Returning here
     * made the attribute mean nothing at `-inline none`, which is a supported way to build and is
     * exactly the "cost depends on a flag" that `resolve/inline.h` argues a directive may not have.
     *
     * The price is a whole-program walk at a level that will splice very little. It is paid in a
     * build that has already turned the optimizer's inliner off, and `worthInlining` refuses every
     * callee below the attribute, so what the walk decides is one thing rather than nothing.
     */

    findRecursion(opt, inliner.recursive);

    auto caller = opt.function;
    auto body = opt.body;

    for(Size round = 0; round < kMaxInlineRounds; round++) {
        /*
         * What the program can still reach, asked again before anything is counted against it - see
         * `reach.cpp`, and [[yana-escape-before-inlining]] item 5 for why a stale answer feeding a
         * threshold is a decided bug rather than a hypothetical one.
         */
        markProgramReachable(opt);
        inliner.countReferences();

        auto inlined = false;

        for(auto modulePointer: opt.program.modules) {
            auto module = opt.decl[modulePointer];

            for(auto pointer: module->functions.contents(opt.decl)) {
                auto function = opt.decl[pointer];

                // A body nothing can reach is not one to copy anything into: it is not emitted, and
                // its own call sites have just stopped being counted.
                if(!function->reachable || !function->ssa) continue;

                inlined = inliner.runFunction(pointer) || inlined;
            }
        }

        if(!inlined) break;
    }

    // And once at the end, so that the per-function loop below this walks what the program actually
    // holds rather than what it held before the last round.
    markProgramReachable(opt);

    opt.function = caller;
    opt.body = body;
}

} // namespace ssa
