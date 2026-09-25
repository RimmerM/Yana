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
 * have addressed is the storage the clone brought with it. A `Host` is not declined for the same
 * reason and one better - it is not even an address - which is what makes `@inline(always)` on the
 * twenty-odd wrappers in `lib/Native/Host.js.yana` cost nothing.
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
 * Two numbers and one exemption, where the shipping stage (deleted in J173) had fourteen terms and
 * a per-target table. That is a starting point rather than a claim: its numbers were calibrated
 * against a particular backend's register pressure ([[yana-inline-limits-vs-llvm]] - budget 20
 * against LLVM's 50, and raising it is +10.7% because the graft enlarges a loop nest the allocator
 * then spills through), and `test/bench/programs` does not compile under this stage yet, so there
 * is nothing here that could rank a second term honestly. What is measurable is the fixture corpus,
 * and the note in `opt/README.md` is what it measured.
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
 * ## The two terms the shipping stage had that this does not, each measured rather than argued
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
 * over there the eight was a base that fourteen bonuses were added to and four penalties subtracted
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
     * Not zero at `Size`, and that is J57's correction: a chain left standing is a closure, a witness
     * slot and an indirect call per link, and over `test/resolve` flattening is worth **5,488 bytes**
     * at that level. What grows a program is copying a body twice; a link of a chain is copied once
     * and takes an indirection with it.
     */
    U32 chain = 0;

    /*
     * Whether `@inline` and `@hot` are read at all.
     *
     * A flag rather than a fourth number, because what they say is not a size: both are "there is a
     * reason to copy this that the body does not show", and a level that will not spend bytes for
     * time has nothing to weigh that against. It used to be `limits.budget` being zero that said
     * this, which stopped being true the moment `Size` got a budget - so the sentence is written
     * down instead of being inferred from a number that now means something else.
     */
    bool hints = false;
};

/*
 * What a body may weigh at a **cold** site - see `worthInlining`, where the argument is.
 *
 * Not in `Budget`, because a budget is a trade between size and speed and this is the one place
 * there is nothing to trade: the author has said the path is rare, so speed is not what the bytes
 * buy. What is left is the one question a cold site may still ask - *is the copy smaller than the
 * call it removes* - and that is a number rather than an argument.
 *
 * **Three, measured J58, and it was one.** The one came from "`sizeOf` counts a call as one, so a
 * callee of one instruction replaces a call with a call": true of the callee and silent about the
 * site, which also pays the argument moves, and about the callee's own prologue and `ret` and the
 * body left standing. It is the same thing `budgetFor(Size)`'s note found - at this tier a call is
 * not free of size - arriving one constant later. Swept over `test/resolve` at `balanced`:
 *
 *     1 -> 283,310    3 -> 283,102    6 -> 283,102    10 -> 283,774    16 -> 283,758
 *
 * and `test/bench/programs` reads 23,137 at 1, 3 and 6 and 23,521 at 10. Three is where it stops
 * paying and ten is where it starts costing; the plateau between them is what a call weighs.
 *
 * **What this number cannot fix is the weighing, and J59 is why no number could.** `byteSpan(I32)`
 * is `difference(bitcast(from), bitcast(from + count))`. It lowers to one `shl` and weighs **ten**
 * here, and the ten is not a body waiting to be folded: `settleCallee` runs the whole per-body
 * optimizer over a callee before any site is weighed against it, so this *is* the settled form.
 * What stands in it is
 *
 *     %8  = metric stride I32        %17 = bitcast %from
 *     %9  = mul %count %8            %18 = bitcast %10
 *     %10 = add %from %9             ...
 *     %15 = metric stride U8         %16 = div %14 %15
 *
 * - two `Metric` reads and the pointer arithmetic over them, and `fold.cpp` declines a `Metric` by
 * ruling: what a type measures is the representation layer's answer and this stage runs once per
 * program rather than once per target. `lower/inst.cpp` turns one into an immediate where the
 * target is in hand and `optimizeLowerFunction` folds the eight instructions around it away, which
 * is a tier below anything this pass can ask. So "fold it earlier" is not a fix that exists - the
 * folder already ran, and running it again answers the same ten - and no cold budget that admits a
 * body of ten is worth its cost elsewhere, which J58 measured at +672 bytes.
 *
 * What *is* true of such a body is a fact about its references rather than about its weight, and
 * that is `kColdCallCost` below.
 */
constexpr U32 kColdBudget = 3;

// How many times a cold block counts in a site's residue - `coldExtra`, J138.
constexpr U32 kColdResidueWeight = 2;

/*
 * What a **call** costs where the callee is about to stop existing - `coldOnlyCallee`'s second term,
 * and the one number in this file that prices the site rather than the body.
 *
 * `sizeOf` counts a call as one instruction, which is what it is and not what it costs. The site
 * also moves an argument per parameter into the places the convention names, and the body on the
 * other side opens with a prologue and ends with a `ret` that no caller counted - the same thing
 * `budgetFor(Size)`'s note found from the other side, and the same thing that made `kColdBudget = 1`
 * wrong. Here it is not a correction to a threshold but the whole of the second term, so it is
 * written as its own number.
 *
 * Swept with the rest of the term held fixed. `.text` bytes over the **392 programs** of
 * `test/resolve` this stage compiles, built the way `-mode exe` builds them:
 *
 *     0 -> 306,536    3 -> 306,504    4 -> 306,504    5 -> 303,400    6 -> 303,448
 *     8 -> 306,760   10 -> 306,760   12 -> 307,192   20 -> 308,856
 *
 * Zero is the term switched off - at one site the inequality holds whatever this number is, and
 * that case is the sole-reference rule which was already there - and it reads the same 306,536 as
 * a build with no term at all, which is the control that says so.
 *
 * **Five, and it is a cliff on both sides rather than a plateau.** Four admits nothing that three
 * did not: `byteSpan` stands at two sites and weighs ten, so it wants five exactly. Eight starts
 * admitting bodies at three and four sites whose copies pay for nothing, and every value above it
 * is worse again. -3,136 bytes at five is **-1.0%** of the corpus. The lowered IR moves with it -
 * 42,236 instructions over the `.mir.lower.expect` goldens to 42,076, and 2,153 emitted function
 * definitions to **2,081**.
 *
 * `kColdBudget` was re-swept underneath this and did not move: 1 and 2 read 303,560, 3 reads
 * 303,400, and 4 and 5 are worse again. The two numbers are not two spellings of one thing -
 * raising the flat budget to 6 or 8 now buys exactly nothing, where J58 measured it buying the
 * wrong bodies for +672.
 *
 * **The population matters and is part of the number.** These 392 are every program this stage
 * compiled; findings.md §104.5's `test/resolve` rows are over the *shared* corpus, the fixtures
 * both stages compiled while there were two, which is smaller. On that corpus the same change read
 * 297,818 -> 294,682 - the identical -3,136 - and the gap against the shipping stage went +6.9% to
 * +5.8%. The two totals are not comparable and §105.5 is why: the shipping column, which no part of
 * this round touched, did not reproduce either.
 */
constexpr U32 kColdCallCost = 5;

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

/*
 * What a callee may cost **on JavaScript**, whatever the level says - Analysis-IR-JS.md §6.10's
 * "a JS compilation can feed the shared inliner JS cost summaries", as one number.
 *
 * The flat budget and nothing else, because that is the one term whose trade is different here. A
 * copy natively is instructions the processor runs, and the reason the flat budget is twenty rather
 * than eight is everything a copy *unlocks* below the inliner: a constant reaching the callee, a
 * bounds test discharged, a slot promoted. All of that still holds here. What does not is the other
 * half of the trade: an out-of-line call on this target is dispatched by an engine that inlines it
 * again at run time, so the time a copy buys is time the host was going to find anyway - while the
 * bytes are download and parse cost that nothing gives back.
 *
 * Measured on `test/base`'s `Base.js` - emitted bytes, and the best of five Node runs of the suite:
 *
 *     budget  12 -> 5,777,178  2.384s      16 (96 sole) -> 5,829,123  2.300s
 *             14 -> 5,805,816  2.296s      20, no hints -> 5,878,124  2.717s
 *             16 -> 5,809,113  2.291s      `-inline size`  -> 5,771,616  2.688s
 *             18 -> 5,858,841  2.348s      `-inline none`  -> 5,969,922  7.453s
 *             20 -> 5,921,807  2.304s      `-inline speed` -> 6,693,352  2.292s
 *
 * **Eight**, and the curve says two things the native one does not. Above it the bytes climb and the
 * time does not move at all - `speed`'s forty is 884 KB larger and reads the same 2.29s - which is
 * the host's own inliner being the thing that decides, and is the whole reason this cap exists.
 * Below it the time turns: the two levels that switch something *off* rather than lowering it -
 * `no hints`, and `-inline size`, which is this budget with the hints gone - are 18% and 17% slower
 * for 38 KB and 4 KB. So `@inline` and `@hot` are worth what they cost here and the flat budget
 * above eight is not.
 *
 * A cap rather than a table, so that `-inline none`, `size` and `speed` still mean on this target
 * what their names say: the other three terms are the level's, and only the one whose trade the
 * host changes is overridden.
 *
 * **Sixteen until J78**, and the number moved because the objective did rather than because the
 * curve did. J72 set it where the *time* stops moving and J77 re-swept it there; J78 was asked for a
 * program at or below the shipping backend's bytes while staying under its time, and re-ran the
 * sweep on J77's instrument beside the second lever §6.10 proposes (see below). Bytes on
 * `test/base`'s `Base.js` and user+system CPU, twenty-one interleaved rounds pinned to one core,
 * against the shipping backend's 5,330,877 bytes and its own 2,326 ms; the control column - a
 * byte-identical copy of the shipping backend's own file - read 1.0009x, which is what says the
 * instrument resolves about 0.1%:
 *
 *     budget   bytes        ratio     CPU ratio
 *          6   5,325,461    0.9990x   0.9912x     (J77's session)
 *          8   5,356,497    1.0048x   0.8943x
 *         10   5,388,522    1.0108x   0.8867x
 *         12   5,401,250    1.0132x   0.8835x     (J77's session)
 *         16   5,424,700    1.0176x   0.8842x
 *
 * Eight is the **smallest program whose time is still comfortably under the shipping backend's** -
 * 0.894x, where six turns it to 0.991x for one more 31 KB. So the step from sixteen to eight is
 * 68,203 bytes for 1.1% of the run and the step from eight to six is 31,036 for another 11%, which
 * is where the curve stops being a trade and starts being a cliff.
 *
 * **The second lever was built, swept against this one in the same session, and is dominated.**
 * §6.10's third summary asks for "repeated source bytes at the number of call sites" - a callee
 * larger than `repeatFree` and called at more than `repeatSites` places refused whatever the budget
 * says - and J77 named it as the shape of the cliff it saw. Built at J78, over the flat budget held
 * at sixteen:
 *
 *     free sites   bytes        ratio     CPU ratio   where the rule sits
 *        6     2   5,333,369    1.0005x   0.9848x     the flat budget only
 *        6     3   5,340,733    1.0019x   0.9780x     "
 *        6     4   5,349,557    1.0035x   0.9843x     "
 *        7     4   5,372,531    1.0078x   -           "
 *        6     4   5,234,246    0.9819x   1.0784x     above every term
 *
 * Two readings, and the second row group is the one that matters. Placed **above every term** - the
 * wording §6.10 uses - it refuses the links of an adaptor chain, which is what
 * `flattenContinuations` copies to turn a `for` over an `iter fn` into one loop: 0.982x the bytes
 * for **1.078x** the time. Restricted to the flat budget, which is the only sentence it is a
 * correction to, it costs 9% of the run for 1.4% of the bytes - and `budget 8` above buys *more*
 * bytes (1.0048x against 1.0035x is within a thousandth) for 0.894x instead of 0.984x.
 *
 * So the term is not kept, and the reason is worth more than the term: **size is a better proxy for
 * "this copy is cheap" than site count is**, because on this corpus the bodies with many sites are
 * the hot ones. `mix32`, `murmurStep`, `MapHashPoint_hash` and `reserve_*` are what the cliff
 * between six and eight is made of, and they are called from everywhere *because* they are the inner
 * loops - so a rule that prices them by how often they appear prices exactly the copies the host
 * cannot make back. A flat budget refusing by size leaves them alone and takes the large-and-rare
 * bodies instead, which is the half of the program that is only bytes.
 */
constexpr U32 kJsBudget = 8;

Budget capped(Budget limits, bool js) {
    if(js && limits.budget > kJsBudget) limits.budget = kJsBudget;
    return limits;
}

/*
 * The cap is applied **once, on the way out**, rather than wrapped around each of the four rows.
 *
 * Four `capped(...)` calls were four chances to add a fifth level and forget one, and the cap is
 * not a property of a level: it is the *pipeline's*, which is what the README's rule about this
 * flag says. A level says what it trades and the target says what it can afford.
 */
static Budget levelBudget(InlineLevel level) {
    switch(level) {
        case InlineLevel::None:
            return Budget { 0, 0, 0, 0, false };

        /*
         * **Measured, J57, and the numbers it replaces were an argument rather than a measurement.**
         *
         * `{0, 96, 96, 0}` was "only the case that cannot grow the program": no flat budget, no
         * chain, a callee admitted through the sole call site and nothing else. Each step of that is
         * true on its own and the total was **381,710 bytes over `test/resolve` against 283,422 at
         * `Balanced`** - `-inline size` produced a *third more code* than the level that trades size
         * for speed, and was slower as well, so there was no reason to ask for it.
         *
         * What the argument missed is that at this tier a call is not free of size. A body left
         * out of line keeps its prologue, its argument moves and its `ret`, the caller keeps the
         * call, and every pass below the inliner stops at the boundary: a constant does not reach
         * the callee, a bounds check inside it is not discharged by a length the caller knows, and
         * `promote` cannot see a slot whose address crossed. Refusing a copy of twelve instructions
         * buys the twelve back and pays for all of that. The sweep is monotone in exactly that
         * direction until the budget passes twenty, and then turns:
         *
         *     budget    0 -> 381,710      chain 0    -> 288,046
         *     budget    8 -> 293,070      chain 240  -> 281,518
         *     budget   16 -> 281,518      hints on   -> 281,726
         *     budget   20 -> 282,862      hints off  -> 281,518
         *     budget   30 -> 296,478
         *
         * So: **sixteen**, which is `Balanced`'s twenty minus the four that were only ever bought
         * with time; the sole call site at ninety-six, because a *move* costs no bytes at all and
         * this is the level that should take every one of them; the chain, because a link left
         * standing is a closure, a witness slot and an indirect call and flattening it is worth
         * 6,528 bytes here; and **no hints**, because `@inline` and `@hot` are the one thing in this
         * table that asks for bytes in exchange for time, which is the trade this level declines.
         *
         * It is now 1,904 bytes *under* `Balanced`, which is the relation the two names promise.
         */
        case InlineLevel::Size:
            return Budget { 16, 96, 96, 240, false };

        case InlineLevel::Balanced:
            return Budget { 20, 76, 96, 240, true };

        case InlineLevel::Speed:
            return Budget { 40, 120, 160, 480, true };
    }

    return Budget();
}

Budget budgetFor(InlineLevel level, bool js) { return capped(levelBudget(level), js); }

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

    /*
     * How many of `calls` stand in a **cold** block - `findColdBlocks`, asked of every caller rather
     * than of the one being walked.
     *
     * A second split for a second rule, and it is the sole-call-site argument counted rather than
     * capped at one: what makes that rule admissible is that the body *moves* instead of being
     * copied, and a body every one of whose callers is a cold block moves too - to more than one
     * destination. `coldOnlyCallee` is where the trade that follows from that is, and the two
     * numbers are separate because a callee with one warm caller is a body that stays whatever this
     * pass does to the cold ones.
     */
    U32 coldCalls = 0;
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

    /*
     * What a site in code that **runs once** may copy - `Function::once`, J154: `-inline size`'s
     * row, whatever level was asked for.
     *
     * The flat budget, the site residue and the `@inline`/`@hot` hints are the three terms that buy
     * time with bytes, and time spent once is not worth buying: `test/base`'s case bodies, its
     * `testInit$`s and its entry were where 75 KB of the gap between `balanced` and `size` sat
     * (J151). The sole call site and the chain stay, because a move costs no bytes at any level.
     *
     * A block of such a body that is in a loop is weighed at `limits`, which is the half of "runs
     * once" that is about the body rather than the declaration - see `findOnceBlocks`.
     */
    Budget onceLimits;

    /*
     * Whether `onceLimits` is a row of its own - native, at `balanced` and `speed`, J154/J156.
     *
     * **Not on JavaScript, J156.** "Time spent once is not worth buying" is true of a program whose
     * machine code is fixed before it runs and false of one a JIT compiles from what it has seen:
     * V8 keeps type feedback per *function*, not per call site, so a call left out of line in a case
     * body feeds the library body's one feedback vector with whatever the case probes - and a
     * test's probes are the edge cases the hot callers never pass. `decodingAtTheEdges` calling
     * `decodeChar("ab", 0)` out of line was the whole of it: one one-byte string reaching
     * `decodeChar`/`pairAt`, whose hot callers pass only two-byte text, made every optimized copy
     * of them polymorphic in the string's encoding, and `test/base`'s `Base.js` ran in 2.20 s of
     * CPU against 1.80 s, `pairAt` alone 18 -> 171 ms. Copied in, the probe has its own feedback.
     * The run-once row bought 15 KB of `Base.js` (0.3%); it is not worth a hazard of that size.
     */
    bool weighOnce;

    // Which functions are in a call-graph cycle - `findRecursion`, computed once before the rounds.
    HashMap<U32, bool> recursive;

    // Per callee, this round's reference counts and whether its body has been settled.
    HashMap<U32, References> references;
    HashMap<U32, bool> settled;

    Array<SsaPtr<Inst>> pending;
    Array<bool> pendingOnce;

    static constexpr U8 kUnset = 0;
    static constexpr U8 kNumber = 1;
    static constexpr U8 kUnknown = 2;

    /*
     * What one of the callee's values holds under the site's constants.
     *
     * One row rather than two arrays in step: the state and the number are read and written together
     * at every one of the four places below, and a pair of parallel arrays is a pair every caller has
     * to index identically.
     */
    struct SiteValue {
        U64 number = 0;
        U8 state = kUnset;
    };

    /*
     * The scratch the two weighings walk over, held here rather than declared inside them.
     *
     * Both run **per call site** - `shouldInline` weighs the callee at every one of them - so a
     * local array is an allocation and a free per call in the program. Everything below is cleared
     * on the way in and keeps its buffer, which is one allocation per body for the whole pass.
     *
     * Two orders rather than one because they are two walks: `sizeOf` measures the callee as
     * written and `sizeAtSite` measures what the site's constants leave of it, and `shouldInline`
     * asks the first and then the second.
     */
    Array<U32> sizeOrder;
    Array<U32> siteOrder;
    Array<SiteValue> siteFacts;
    IndexSet siteLive;

    // Per entry argument of the callee being weighed, the `Aggregate` the site's argument denotes -
    // `builtContents` - or null. See `siteField`, the one reader.
    SmallArray<Value*, 8> siteRecords;
    Block* siteEntry = nullptr;

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
        reversePostorder(base(), *body, sizeOrder);

        U32 size = 0;
        for(auto index: sizeOrder) size += weighBlock(*body->block(base(), BlockId(index)));

        return size;
    }

    // What of that is **cold** - the blocks `findColdBlocks` marked in the callee's own body, which
    // lead nowhere but to a `@cold` call. Read only by the flat budget; see the note there.
    U32 coldSizeOf(mir::Function& function) {
        auto body = base()[function.ssa];
        reversePostorder(base(), *body, sizeOrder);

        U32 size = 0;
        for(auto index: sizeOrder) {
            auto& block = *body->block(base(), BlockId(index));
            if(block.cold) size += weighBlock(block);
        }

        return size;
    }

    /*
     * What a residue's **cold** blocks add on top of their plain weight - J138, J94 §6.1's cause.
     *
     * `sizeAtSite` credits a site with what its constants fold away, and that credit is what copies
     * a callee above the flat budget. A cold arm the constants leave standing is no part of that
     * win: it runs rarely by the author's say-so, so nothing the copy lets the caller fold there is
     * worth anything, and what it costs is the whole arm at every site. `checkEqual(actual,
     * expected)` is the case - `expected` is a constant at nearly every assertion, the residue
     * folded the comparison's other half and came in under the budget, and the arm that formats
     * the claim and hands it to `@cold` `recordAt` was copied 400 times into `test/base`.
     *
     * Weighed here and **not** in the flat budget. Built both ways: pricing the arms in `sizeOf` as
     * well kept small callees out of line whose copy was no larger than the call it replaced -
     * `test/base` 1,088,728 bytes against 1,043,176, and the thirteen bench programs 23,489 against
     * 23,297, at a CPU time inside the instrument's resolution. The weight itself is not a tuned
     * number: 2, 3, 5 and 9 give byte-identical `test/base`, because an arm surviving is always
     * enough to push the residue over, and 1 is the J137 compiler.
     */
    U32 coldExtra(Block& block, Array<SiteValue>* decided) {
        return block.cold ? weighBlock(block, decided) * (kColdResidueWeight - 1) : 0;
    }

    /*
     * What the callee weighs **as the site's constants leave it** - the term J63 added, and the one
     * the file comment above used to name as missing.
     *
     * ## Why a second size
     *
     * `sizeOf` measures the callee as written, which is the right number for a site that knows
     * nothing about it and the wrong one for a site that decides half of it. The shape this exists
     * for is a **ladder**: a library body that dispatches on a count, written for a count nobody
     * knows, whose every threshold folds where the count is a literal.
     *
     * `compareMemory` in `lib/Native/Memory.native.yana` is the case. It weighs **149** at
     * `CodeUnit`, which is above every budget in the table and above the ceiling at two of the three
     * levels, and at a five-unit string literal what survives the copy is about twenty-five: both
     * vector loops have entry tests that are false at that count, `unenteredLoops` writes them down
     * and `cleanCfg` takes the bodies, and what is left is the one tail the length selects. A
     * `match` over string literals is a chain of those calls, and refusing every one of them on the
     * strength of a body nobody will emit is what left `compareMemory` at 10.95% of the lexer's x64
     * profile and absent from its LLVM one.
     *
     * The mirror of it is already here: `isBlockMoveLadder` **refuses** `copyMemory` where the count
     * is not constant, on the reading that a copy buys nothing when no threshold folds. This is that
     * sentence read the other way round, and generally rather than by declaration.
     *
     * ## The estimate
     *
     * Conditional constant propagation over the callee's own blocks, seeded with the constants the
     * site passes - a block is measured only where it is reachable under those. Optimistic and
     * iterated, which is what makes a **loop** come out right: the body of a loop whose header test
     * the entry edge decides false is never marked reachable, so the latch never is either, so the
     * back edge never contributes to the header's arguments and the answer stands. That is
     * `unenteredLoops`' proof arrived at from the other end, and the estimate is honest exactly
     * because the pass that will do the work is the one it models.
     *
     * Nothing is written and nothing is cached: this runs only for a callee the flat budget has
     * already declined and only where the site passes a constant at all, which over the corpus is a
     * handful of sites per program.
     *
     * **What it does not model** is the folding a constant enables *inside* a block - an address
     * that becomes an immediate, a comparison that becomes a jump. So the answer is an over-estimate
     * of the residue, which is the direction an inliner's estimate should err in.
     */
    U32 sizeAtSite(InstCall& call, mir::Function& function) {
        auto body = base()[function.ssa];
        if(!body) return ~U32(0);

        auto entry = base()[body->entry];
        if(entry->argumentCount() != call.arguments.size()) return ~U32(0);

        // Nothing to estimate where the site knows nothing. Asked first, because everything below it
        // allocates.
        auto supplied = false;
        siteEntry = entry;
        siteRecords.clear();
        for(Size i = 0; i < call.arguments.size(); i++) {
            auto used = call.arguments.get(base(), i);
            auto value = used.isSet() ? base()[used.value()] : nullptr;
            if(value && constantValueOf(opt, value)) supplied = true;

            // Asked of a teardown's site only: `builtContents` reads the caller's slots
            // (`aggregateFillOf`), which every splice invalidates, and asking it at every declined
            // site cost 48% of the compile of `test/base` in the first measurement of this.
            auto record = function.teardown ? builtContents(opt, value, 0) : nullptr;
            if(record && base()[record->definer]->kind != Inst::Aggregate) record = nullptr;
            if(record) supplied = true;
            siteRecords.push(record);
        }

        if(!supplied) return ~U32(0);

        reversePostorder(base(), *body, siteOrder);

        // The two lattices, both of which only ever fall: a block becomes reachable and stays so, and
        // a value goes from unset to a number to unknown. That is what makes the sweep below
        // terminate without a worklist.
        auto& facts = siteFacts;
        auto& live = siteLive;

        facts.clear();
        for(U32 i = 0, values = U32(body->values.size()); i < values; i++) facts.push(SiteValue());

        live.reset(U32(body->blocks.size()));
        live.set(entry->index.index, true);

        for(Size i = 0; i < entry->argumentCount(); i++) {
            auto& fact = facts[base()[entry->argumentAt(base(), i)]->id.index];
            auto used = call.arguments.get(base(), i);
            auto found = used.isSet() ? constantValueOf(opt, base()[used.value()]) : Nothing();

            fact.state = found ? kNumber : kUnknown;
            if(found) fact.number = found.unwrap();
        }

        for(U32 round = 0; round < kSiteRounds; round++) {
            auto moved = false;
            for(auto index: siteOrder) {
                if(!live[index]) continue;
                if(sweepBlock(*body->block(base(), BlockId(index)), live, facts,
                              index == entry->index.index)) {
                    moved = true;
                }
            }

            if(!moved) break;
        }

        U32 size = 0;
        for(auto index: siteOrder) {
            if(!live[index]) continue;
            size += weighBlock(*body->block(base(), BlockId(index)), &facts);
            size += coldExtra(*body->block(base(), BlockId(index)), &facts);
        }

        return size;
    }

    /*
     * How many times the sweep above is run over the block list.
     *
     * Three, and the first one answers every shape this exists for: a straight-line ladder settles in
     * one pass, and a loop whose body never becomes reachable settles in one too, because the latch
     * it would have heard from is never visited. What the second and third buy is a value that
     * becomes *unknown* late - a merge whose second predecessor turned out reachable - and lowering
     * one can only shrink the live set, never grow it. A cap rather than a fixpoint because this is
     * an estimate, and one a round short is one that over-estimates.
     */
    static constexpr U32 kSiteRounds = 3;

    /*
     * The number a callee's read of one of its **parameters' fields** takes at this site - the
     * callee-side half of `builtContents` (analysis.h).
     *
     * The address is walked up to the entry argument it projects - field steps, and a `Guaranteed`
     * load, which is a borrow of the same storage - and then down the site's record along the same
     * fields.
     */
    Maybe<U64> siteField(Value& address) {
        SmallArray<InstFieldAddress*, 4> path;
        auto at = &address;

        for(U32 depth = 0; at && at->definer; depth++) {
            if(depth > 6) return Nothing();

            auto inst = base()[at->definer];
            if(inst->kind == Inst::FieldAddress) {
                path.push((InstFieldAddress*)inst);
                at = base()[((InstFieldAddress*)inst)->base.value()];
                continue;
            }

            if(inst->kind == Inst::Load && inst->results.size() &&
               base()[inst->results.get(base(), 0)]->convention == Convention::Guaranteed) {
                at = base()[((InstLoad*)inst)->address.value()];
                continue;
            }

            return Nothing();
        }

        if(!at || !path.size() || !at->block) return Nothing();

        // An entry argument, by position - the site's arguments are in the same order.
        auto block = base()[at->block];
        if(block != siteEntry) return Nothing();

        Value* record = nullptr;
        for(Size i = 0; i < block->argumentCount() && i < siteRecords.size(); i++) {
            if(base()[block->argumentAt(base(), i)] == at) record = siteRecords[i];
        }

        for(Size i = path.size(); record && i-- > 0;) {
            record = builtContents(opt, builtComponent(opt, record, *path[i]), 0);
        }

        return record ? constantValueOf(opt, record) : Nothing();
    }

    // The number a value holds under the site's constants, reading the table first and the IR second
    // - a `Const` defined in a block the sweep has not reached is still that constant.
    Maybe<U64> numberOf(Array<SiteValue>& facts, Use used) {
        if(!used.isSet()) return Nothing();

        auto value = base()[used.value()];
        auto index = value->id.index;

        if(index < facts.size() && facts[index].state == kNumber) return Just(facts[index].number);
        if(index < facts.size() && facts[index].state == kUnknown) return Nothing();

        return constantValueOf(opt, value);
    }

    // One value lowered, and whether that moved the lattice.
    bool lower(Array<SiteValue>& facts, U32 index, Maybe<U64> found) {
        auto& fact = facts[index];
        auto want = found ? kNumber : kUnknown;
        if(fact.state == want && (!found || fact.number == found.unwrap())) return false;

        // Never back up the lattice: a value that has been unknown stays unknown, which is what makes
        // the rounds terminate.
        if(fact.state == kUnknown) return false;

        fact.state = want;
        if(found) fact.number = found.unwrap();

        return true;
    }

    /*
     * One block under the site's constants: its arguments answered by its reachable predecessors, its
     * instructions folded where both operands are numbers, and the successors its terminator can
     * actually reach marked live.
     */
    bool sweepBlock(Block& block, IndexSet& live, Array<SiteValue>& facts, bool isEntry) {
        auto moved = false;

        // The entry's arguments are the site's and are not answered by an edge - there is none.
        if(!isEntry) {
            for(Size i = 0; i < block.argumentCount(); i++) {
                auto index = base()[block.argumentAt(base(), i)]->id.index;
                Maybe<U64> agreed = Nothing();
                auto any = false;
                auto differ = false;

                for(auto pointer: block.incoming(base())) {
                    auto from = base()[pointer];
                    if(!live[from->index.index]) continue;

                    auto end = from->terminator(base());
                    if(!end) { differ = true; continue; }

                    forEachEdge(base(), *end, [&](Edge& edge) {
                        if(!edge.target || base()[edge.target] != &block) return;
                        if(i >= edge.arguments.size()) { differ = true; return; }

                        auto found = numberOf(facts, edge.arguments.get(base(), i));
                        if(!found) { differ = true; return; }

                        if(!any) { agreed = found; any = true; }
                        else if(agreed.unwrap() != found.unwrap()) differ = true;
                    });
                }

                if(differ || !any) moved |= lower(facts, index, Nothing());
                else moved |= lower(facts, index, agreed);
            }
        }

        for(auto pointer: block.instructions(base())) {
            auto& inst = *base()[pointer];
            if(isTerminator(inst.kind)) continue;
            if(!inst.results.size()) continue;

            auto result = base()[inst.results.get(base(), 0)];
            auto index = result->id.index;

            if(inst.kind == Inst::Const) {
                moved |= lower(facts, index, constantValueOf(opt, result));
                continue;
            }

            if(inst.kind == Inst::Load && result->convention == Convention::Trivial) {
                if(auto found = siteField(*base()[((InstLoad&)inst).address.value()])) {
                    moved |= lower(facts, index, found);
                    continue;
                }
            }

            if(inst.kind == Inst::Binary && inst.results.size() == 1) {
                auto& binary = (InstBinary&)inst;
                auto left = numberOf(facts, binary.lhs);
                auto right = numberOf(facts, binary.rhs);

                if(left && right) {
                    moved |= lower(facts, index,
                                   binaryOfConstants(opt, binary, left.unwrap(), right.unwrap()));
                    continue;
                }
            }

            for(auto resultPointer: inst.results.contents(base())) {
                moved |= lower(facts, base()[resultPointer]->id.index, Nothing());
            }
        }

        auto end = block.terminator(base());
        if(!end) return moved;

        // A branch whose condition the site's constants decide reaches one arm; everything else
        // reaches all of its edges.
        Block* only = nullptr;
        if(end->kind == Inst::Branch) {
            auto& branch = (InstBranch&)*end;
            if(auto decided = numberOf(facts, branch.condition)) {
                auto& arm = decided.unwrap() ? branch.whenTrue : branch.whenFalse;
                if(arm.target) only = base()[arm.target];
            }
        }

        forEachEdge(base(), *end, [&](Edge& edge) {
            if(!edge.target) return;

            auto target = base()[edge.target];
            if(only && target != only) return;
            if(live[target->index.index]) return;

            live.set(target->index.index, true);
            moved = true;
        });

        return moved;
    }

    /*
     * What one block weighs, by the rule `sizeOf` states - the two callers share it so that a residue
     * and a whole are measured the same way and the two numbers are comparable.
     *
     * `decided` is the site estimate's table or null. An instruction whose result the propagation
     * came out with a number for is one the folder will replace with a `Const`, and a `Const` is
     * already free above - so counting it would price the arithmetic a constant argument removes as
     * though it were still there, which is most of what this estimate is for. `compareMemory`'s
     * preamble is four such instructions and its two loop bounds are two more.
     */
    U32 weighBlock(Block& block, Array<SiteValue>* decided = nullptr) {
        U32 size = 0;

        for(auto pointer: block.instructions(base())) {
            auto& inst = *base()[pointer];
            if(decided && inst.results.size() == 1) {
                auto index = base()[inst.results.get(base(), 0)]->id.index;
                if(index < decided->size() && (*decided)[index].state == kNumber) continue;
            }

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
                 * expands to a comparison and a branch per case unless the backend finds a table for
                 * it. Understating it is how the inliner admits a body that then explodes.
                 */
                case Inst::Switch:
                    size += U32(((InstSwitch&)*base()[pointer]).cases.size());
                    break;

                default:
                    size++;
                    break;
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

        /*
         * A `@cold` body is cold **all the way through**, which is the third half of the attribute
         * and the one the two above could not state.
         *
         * `@noinline` keeps the body where it is and the caller-block rule keeps other bodies out of
         * the *caller*; neither says anything about what is expanded **into** the rare body itself.
         * `growArray` is where that shows: it is `@noinline`, it is one doubling per amortized push,
         * and it was 45 bytes in the shipping stage and **374** here, because `resize`,
         * `moveMemory` and `allocateHeap` were each worth copying into a body nothing weighed as
         * rare. Every byte of that is on a path the author has already said is not worth spending
         * time on, so there is nothing to trade it against.
         *
         * Seeded rather than tested at each site, so that the fixed point below and `inColdBlock`
         * read one answer: a body that is cold by declaration and a block that is cold by what it
         * reaches are the same fact to everything downstream.
         *
         * **What the seeding costs, and where that is paid back - J59.** Every call in the body is
         * now weighed at a cold site, which is the point, and the answer for a callee too large to
         * copy is that the *call* stays. For a callee whose callers are all cold bodies, that leaves
         * an out-of-line function as well: `growArray` is `@cold`, `byteSpan` is called only from
         * `growArray`, and six one-instruction `byteSpan` bodies stood in the corpus because of it -
         * the same "refusing the copy leaves the call **and** a body" the note below gives for why
         * `checkFailed` is deliberately not `@cold`. The seeding is right and the answer is not to
         * weaken it; `worthInlining`'s `coldOnlyCallee` is where the case is met, by asking of the
         * callee what this asks of the block.
         */
        if(opt.function->cold) {
            for(U32 i = 0; i < body.blocks.size(); i++) coldBlocks[i] = true;
        }

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

    /*
     * Which blocks of a body that runs once are in no loop - `Function::once`, J154.
     *
     * Empty for any other body. Asked of the natural loops rather than of the cold-block walk's
     * shape, because what matters is how often the block runs and a block is run more than once per
     * call only by being in a cycle. An irreducible cycle has no natural loop, so a call in one is
     * weighed at the run-once row - and one does reach this tier: J154 §4 found an iterator object's
     * resume switch inlined into its consumer, which enters a loop in the middle, and a `@test` case
     * that steps an object in a loop is that shape. What that risks is time: a call the cycle runs
     * many times inlined at the larger budget, or kept out of line where it was worth inlining. It
     * never risks correctness: an inlining decision changes what is emitted, not what it computes.
     *
     * **The loop test is measured, J154.** Weighing a whole case body at `onceLimits` takes
     * `test/base` 1,005,656 -> 970,392 bytes and costs **+30 M** instructions of its run (13.796 G);
     * leaving the loops at `limits` is 982,472 and **-5 M**. A `@test` case is a loop over 0x110000
     * code points as often as it is a list of assertions, so the second is the rule.
     *
     * Read per *call* rather than per block at the site - `pendingOnce` - because a splice moves the
     * rest of a block into a new one, and a table indexed by block would read every call after the
     * first in a straight-line case body as outside it.
     */
    Array<bool> onceBlocks;

    void findOnceBlocks(mir::Function& function) {
        onceBlocks.clear();
        if(!function.once || !weighOnce) return;

        for(U32 i = 0; i < opt.body->blocks.size(); i++) onceBlocks.push(true);

        for(auto& loop: loopsOf(opt)) {
            loop.contains.forEach([&](U32 index) {
                if(index < onceBlocks.size()) onceBlocks[index] = false;
            });
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
     * Whether this callee is one of the two block-move ladders - `known.def`'s `copyMemory` and
     * `moveMemory` rows, which name them for exactly the reason this reads them: what they are is
     * not visible in the body.
     *
     * Through `specializes`, as `summary.cpp` does, so that a specialization of one answers as the
     * generic it runs. Neither is generic today; the test costs a load and stops the rule being
     * wrong the day one is.
     */
    bool isBlockMoveLadder(DeclPtr<mir::Function> pointer) {
        if(!pointer) return false;

        auto callee = decl()[pointer];
        auto root = callee->specializes ? callee->specializes : pointer;

        return root == opt.program.roles.function(intrinsic::Known::copyMemory)
            || root == opt.program.roles.function(intrinsic::Known::moveMemory);
    }

    /*
     * Whether a teardown call's subject was assembled in this body - `builtContents` finds the
     * `Aggregate` it was built from, through the storage it was kept in. See the note in
     * `worthInlining`, the one reader.
     */
    bool subjectBuiltHere(InstCall& call) {
        if(!call.arguments.size()) return false;

        auto used = call.arguments.get(base(), 0);
        return used.isSet() && builtContents(opt, base()[used.value()], 0) != nullptr;
    }

    /*
     * Whether the copy is worth making - the two numbers and the exemption.
     *
     * `@inline(always)` reaching here at all means the conversion's graft declined it, which happens
     * at eight expansions deep and nowhere else. It is **weighed against nothing** - J49, and the
     * two reasons are that it is a directive and that the graft is the other half of the same one.
     *
     * The graft applies no size test whatever, so a ceiling here made the directive mean one thing
     * for a call the conversion reached and another for a call eight deep that it did not. Worse,
     * it made it mean *nothing at all* at `-inline none`, where the ceiling is zero and the
     * early-out above refused every callee before the attribute was read - and `-inline none` is a
     * supported way to build, which is the whole argument the shipping stage's `resolve/inline.h`
     * made for its graft living in the resolver: a body whose cost depends on an optimization flag
     * is the thing the directive exists to prevent.
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
    bool worthInlining(InstCall& call, bool flattening, bool cold, bool once) {
        // The site's row: `onceLimits` in straight-line code of a body that runs once. A local of
        // the member's name, so that every rule below reads the site's row and none can forget to.
        auto& limits = once ? onceLimits : this->limits;

        auto pointer = call.callee;
        auto callee = decl()[pointer];

        // `@noinline`, which is a directive rather than a weight: declining to inline is always
        // possible, so this is the one thing a declaration says here that is never traded against a
        // size. Asked before the numbers for that reason, and not after them.
        if(callee->noInline) return false;

        /*
         * **The block-move ladders, which are a dispatch and not a copy.** J60.
         *
         * `copyMemory` and `moveMemory` in `lib/Native/Memory.native.yana` are the size dispatch
         * over a block move written for a count nobody knows: a vector pair under `width * 4`,
         * `blockShort`, the ascending and descending loops, and `rep movsb` past the length where
         * its flat rate overtakes them. Nine blocks and sixty-two lowered instructions, and `sizeOf`
         * weighs that at a number `balanced` clears - so every site was getting its own copy of the
         * whole ladder.
         *
         * **What a copy of it buys is a `call` and a `ret`.** Not one comparison of the dispatch is
         * removed: the count is what selects the arm, the count is not known here, and the arms are
         * separate functions the copy does not bring with it. That is the entire trade, and it is
         * two hundred bytes against ten.
         *
         * So the rule is about the *count*, not the declaration - where the site does pass a
         * constant every threshold folds and what is left is the one arm the length selects, which
         * is worth having and is why this is not `@noinline` in the library. Two further reasons it
         * is not: the attribute would bind the JS build, where `copyMemory` claims `blockCopy`
         * itself and is one node rather than a ladder, and it would have bound the shipping stage,
         * which already declined the same body on its ceiling and was the control this was
         * measured against.
         *
         * A count that is `sizeOf(T)` does **not** read as constant here, and correctly so: it is a
         * `Metric`, `fold.cpp` declines those by ruling (J59), and until the lower tier folds it
         * nothing in the spliced ladder would fold either. So this admits the source literal and
         * refuses the rest, which is what the tier can honestly tell apart.
         */
        if(isBlockMoveLadder(pointer)) {
            auto count = call.arguments.size() == 3
                ? opt.base[call.arguments.get(base(), 2).value()] : nullptr;

            if(!constantValueOf(opt, count)) return false;
        }

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
         * switch, and it is the same call the shipping stage's located build made.
         *
         * **Except a `checkCondition` the optimizer has already answered** - J168. A condition that
         * is a constant zero is a check that cannot fail, and expanding it leaves nothing, so there is
         * no location to lose; refusing it left a call that does nothing and, behind it, the whole
         * reporting path rooted for it. What `CheckLocation.yana`'s array literal's capacity check
         * was.
         */
        if(opt.context.settings.checkLocations &&
           (pointer == opt.program.roles.function(intrinsic::Known::checkFailed) ||
            pointer == opt.program.roles.function(intrinsic::Known::checkCondition))) {
            auto decided = pointer == opt.program.roles.function(intrinsic::Known::checkCondition) &&
                           call.arguments.size() == 1 && call.arguments.get(base(), 0).isSet();
            if(decided) {
                auto known = constantValueOf(opt, base()[call.arguments.get(base(), 0).value()]);
                decided = known && known.unwrap() == 0;
            }

            // Copied whatever the budgets say: what the copy folds to is nothing at all.
            return decided;
        }
        // And the directive on the other side, above every number for the reason the note has.
        if(callee->alwaysInline) return true;

        if(!limits.ceiling) return false;

        auto size = sizeOf(*callee);

        /*
         * A **teardown** is copied only where its subject was built in this body - J137.
         *
         * Since `opt/teardown.cpp` a drop whose teardown is an authored member is a call of it, so
         * this is the first time the inliner is asked about one. What a reclaim does is decided by
         * the value it is handed: `[5, 6, 7]` placed on the frame carries a flag saying the run is
         * borrowed and its whole reclaim folds to nothing, and an array that arrived as a parameter
         * carries a flag nobody here knows and its reclaim is a test and a `freeHeap`. So the copy
         * is worth making where the aggregate is in this body for the folds to read - and nowhere
         * else, where it is the same test and release the call ran, twenty lowered instructions in
         * place of one, beside an out-of-line body that stays: every teardown is a root of `reach`
         * because the lowering's own walks call it (J94 §6.2's D).
         *
         * The first J137 build copied them all, and it is the measurement that says so: the corpus
         * grew 4.5%, and `Lambda.yana`'s `consume` grew past the budget and stopped being copied
         * into `main`, where its array had been frame-placed - so the run went to the heap.
         *
         * Not even at `kColdBudget`, which is the second thing that build found: a settled
         * `Array.reclaim` weighs three - the flag's load, the comparison and the call - so the
         * "no larger than the call" rule admitted it, and `freeHeap` was then copied in behind it
         * by the next round on its own weight.
         */
        if(callee->teardown && !subjectBuiltHere(call)) return false;

        /*
         * And the same two numbers asked of the **site** - §J63, and `sizeAtSite` is where the whole
         * argument is.
         *
         * `size` is the body as written, which is the right number for a site that knows nothing
         * about it. Where the site passes constants the callee's own control flow reads, what gets
         * copied is smaller than that - a ladder handed a literal count is one arm and no ladder -
         * and the two numbers the copy is measured by are the two that are about what gets copied.
         * The three below are not: the sole call site, the cold-only callee and the hint are each a
         * statement about the **declaration** having a reason to be copied, so each keeps reading
         * the size of the declaration.
         *
         * Asked only where the flat budget has already declined, so a callee that was going to be
         * copied anyway pays nothing for this.
         */
        auto weighed = size;
        if(size > limits.budget) {
            auto residue = sizeAtSite(call, *callee);
            if(residue < weighed) weighed = residue;
        }

        if(weighed > limits.ceiling) return false;

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
         * The **cold-only callee** - the sole-reference rule with the one at the end of it counted
         * rather than required, and the term J59 added.
         *
         * What makes `solelyReferenced` admissible is not that there is one call. It is that the
         * body *moves*: the last reference goes with the splice and `markProgramReachable` drops the
         * original, so the program pays the copy instead of the body rather than as well as it. A
         * callee every one of whose references is a call from a cold block moves too - to `n`
         * destinations instead of one - and the trade that follows is arithmetic rather than a
         * judgement:
         *
         *     inlined     n * size
         *     left alone  size + n * kColdCallCost
         *
         * The second term is what the call sites cost that the copy does not: the argument moves at
         * each site, and the callee's prologue and `ret` amortized over them. So the copy is worth
         * making when `(n - 1) * size <= n * kColdCallCost`, which at `n == 1` is `0 <= cost` - the
         * sole-reference rule, recovered rather than restated.
         *
         * **This is what `@cold` regressed**, and `byteSpan` is the case. It is
         * `difference(bitcast(from), bitcast(from + count))`, which lowers to one `shl` and weighs
         * **ten** at this tier: two `Metric` reads that `fold.cpp` declines by ruling - a
         * measurement is the representation layer's answer and this stage runs once per program -
         * and eight instructions of pointer arithmetic that fold only once those are immediates.
         * Ten is above `kColdBudget` and will stay above it however early the folder is run, because
         * the folder is not what is missing. What *is* true of the body is the thing this term reads
         * instead: `growArray` is its only caller, `growArray` is `@cold`, and refusing the copy
         * left the call **and** a one-instruction function - the same "leaf" the note in
         * `findColdBlocks` gives for why `checkFailed` is deliberately not `@cold`.
         *
         * `size <= limits.soleBudget` bounds it at the number the sole-reference rule already uses,
         * and the inequality above is what stops that number meaning anything at a callee with many
         * sites: at `soleBudget` of 96 a body of 96 is admitted at one site and at no other.
         */
        auto coldOnlyCallee = [&] {
            auto found = references.getValue(pointer.offset);
            if(!found) return false;

            auto counts = found.unwrap();

            // Every reference a call, and every call cold. A function value, a witness slot or a
            // teardown is a reference the lowering follows and this pass cannot see removed, so one
            // of those is a body that stays whatever happens to the calls - exactly as above.
            if(counts.other || !counts.calls || counts.coldCalls != counts.calls) return false;
            if(callee->teardown || pointer == opt.program.entry) return false;

            if(size > limits.soleBudget) return false;

            return (counts.calls - 1) * size <= counts.calls * kColdCallCost;
        };

        /*
         * **A cold site is compiled the way `-inline size` compiles everything**, which is a
         * sentence the table already contains rather than a fourth number: `budgetFor(Size)` is
         * `{0, 96, 96, 0}` and its note says why - "only the case that cannot grow the program".
         * That is what an author writing `@cold` asked for, one path at a time.
         *
         * Three terms, and all of them are "this cannot grow the program". `kColdBudget` admits a
         * body no larger than the call it replaces; the sole reference admits a body that is being
         * moved rather than copied; the cold-only callee is that same move to more than one
         * destination. What none of them admits is the message an assertion's failure arm builds -
         * `String.ofCapacity`, a `push` per piece, a `show` per value, each with callers elsewhere -
         * and that is the whole of what the attribute was written for. The third term is the reason
         * that sentence is now load-bearing rather than incidental: *elsewhere* is what it tests.
         *
         * The three terms it skips are the three that are reasons to *grow* the program: the flat
         * budget, the `@inline`/`@hot` hint and the chain. Each of them buys time, and time on a
         * path the author has said is rare is what there is nothing to trade for.
         */
        if(cold) return size <= kColdBudget || solelyReferenced() || coldOnlyCallee();

        /*
         * The flat budget, and **what it may bring with it cold** - J171.
         *
         * What a copy under this budget buys is its warm path: a comparison the caller's constants
         * decide, a call and its prologue gone. A cold arm copied with it buys nothing - it runs
         * rarely by the author's say-so - and costs its whole weight at every site. So the copy may
         * bring a cold arm only as large as a cold site would itself accept, `kColdBudget`: the call
         * to `checkFailed` behind a bounds test, and not the message an assertion builds.
         *
         * **`-inline speed` is where this showed.** `checkEqual(I64)` settles at 38, of which the
         * arm that formats "expected {expected}, got {actual}" is 32 and the comparison 6. Before
         * `String.view` was grafted it settled at 50 (44 cold), just above `speed`'s 40, and the
         * graft took twelve off the arm - so every assertion in `test/base` copied its failure
         * message, 1,037,544 -> 1,112,120 bytes for the same time (J170 §3). Nothing about the
         * body's warm path had changed; the budget was pricing the arm as though it were the thing
         * being bought.
         *
         * `weighed < size` is the site estimate, which has already taken out the arms the site's
         * constants decide and priced the ones left at `kColdResidueWeight` (`coldExtra`), so it is
         * its own answer. A body that passes the flat number with too large an arm gets that estimate
         * here instead, which is the site that decides its test at compile time.
         *
         * Not the J138 rule, which priced cold arms double in `sizeOf` itself and kept small callees
         * out of line whose whole copy was no larger than the call. This reads the arm on its own and
         * leaves a body with a one-call arm exactly where it was.
         */
        if(weighed <= limits.budget) {
            if(weighed < size || coldSizeOf(*callee) <= kColdBudget) return true;

            auto residue = sizeAtSite(call, *callee);
            if(residue <= limits.budget) return true;
        }

        /*
         * And the residue weighed at `soleBudget`, which is the hint's number read for the hint's own
         * sentence: *there is a reason to copy this body that the body itself does not show*. A site
         * whose constants delete most of a callee is exactly that reason, and it is a better one than
         * an attribute, because the pass computed it rather than being told.
         *
         * Gated on `limits.hints` for the same reason the two attributes below are: this is the row
         * that asks for bytes in exchange for time, which is the trade `InlineLevel::Size` declines.
         * `weighed < size` is what keeps it from being a second reading of the flat budget - a callee
         * the site decides nothing about is one this has nothing to say about.
         *
         * **Measured over `test/resolve`'s 442 executables and `test/bench/lexer`**, at `balanced`:
         *
         *     no site estimate        1,740,032 bytes    idiom/hand x64 32.44 ns/token
         *     residue at `budget`     1,739,056          32.43
         *     residue at `soleBudget` 1,748,808          30.98
         *
         * So the flat budget reading is a small size win on its own and the `soleBudget` one is
         * **4.5% of the lexer's x64 time for 0.56% of its size**, which is the trade this level's
         * name promises. The LLVM rows do not move, and should not: that backend performs this
         * estimate itself, so what the copy buys there is already bought.
         */
        if(limits.hints && weighed < size && weighed <= limits.soleBudget) return true;

        // A link of a chain, at `soleBudget` and for the same sentence the hint is weighed by: there
        // is a reason to copy this body that the body itself does not show. Gated on the chain's own
        // budget rather than on the flat one, which is what lets `Size` flatten and not copy.
        if(flattening && limits.chain && size <= limits.soleBudget) return true;

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
        if(callee->inlineHint && limits.hints && size <= limits.soleBudget) return true;

        // `@hot`, at the same number and for the same sentence: it is a reason to copy a body that
        // the body itself does not show. A hint rather than a directive, so it reads the budget and
        // is declined at `Size` exactly as the line above it is.
        if(callee->hot && limits.hints && size <= limits.soleBudget) return true;

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
             * A run request, cloned **with its placement bit** - and clearing it was a defect rather
             * than a decision, which is what J51 found.
             *
             * The bit and the descriptor's `ownsHeap` field are **one statement made twice**, and
             * `place.cpp` writes both in the same rewrite for exactly that reason: a request marked
             * placed is frame storage and its descriptor says `runBorrowed`, so nothing hands it
             * back. The clone copied the descriptor - it is an ordinary `Aggregate` over an ordinary
             * `const` - and reset the request, which left the pair saying two different things: the
             * lowering expands an unplaced request into `allocateHeap`, and `Reclaim` reads
             * `runBorrowed` and frees nothing. **Thirty allocations over fifteen `test/resolve`
             * fixtures leaked that way**, and `place.cpp` then declined to repair any of them, since
             * `soleDescriptor` asks for a flag that still claims the allocator.
             *
             * What the note here used to argue for is still true and still happens: a request the
             * callee left **unplaced** arrives unplaced, and `place.cpp` runs after inlining and
             * answers it for the frame it ended up in - `leaving() = [7, 8, 9]` is the allocator's
             * under its own name and the caller's frame once it is spliced in. What changes is only
             * the other direction, and it needs no second proof: a run the callee placed is one
             * `inFrame` showed reaches no return, no store of its address and no call that keeps it,
             * so the cloned uses are the same uses and the caller's frame outlives the callee's by
             * construction.
             */
            case Inst::RunSlots: {
                auto& slots = (InstRunSlots&)from;
                auto request = (InstRunSlots*)editor.addRunSlots(at, slots.element, slots.capacity);

                request->placed = slots.placed;
                made = request;
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
                                        results ? convention() : Convention::Trivial, results != 0,
                                        written.immediate);
                break;
            }

            case Inst::Host: {
                auto& written = (InstHost&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.arguments, arguments);

                made = editor.addHost(at, written.op, written.member, arguments.pointer(),
                                      arguments.size(), results ? occurrence() : Occurrence(),
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
                                        results ? convention() : Convention::Trivial, results != 0,
                                        written.isVolatile);
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
                                      convention(), ((InstLoad&)from).overread,
                                      ((InstLoad&)from).isVolatile);
                break;

            case Inst::Copy:
                made = editor.addCopy(at, mapUse(clone, ((InstCopy&)from).address), occurrence(),
                                      convention());
                break;

            case Inst::Store: {
                auto& written = (InstStore&)from;
                made = editor.addStore(at, mapUse(clone, written.address),
                                       mapUse(clone, written.value), written.isVolatile);
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
             * re-decide the placement: `escape-before-inlining` in the shipping stage was a demote
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
             * A multi-way branch - the cases, the fallback, and **each edge's own** argument list.
             *
             * This used to read one list off the fallback and give it to every edge, on the reading
             * that a `switch` is built with one and that `promote`'s block argument is a *branch*
             * shape. It is not: `promote` places an argument where a slot's writes meet and pushes
             * the operand onto the arms that reach that block, and a `switch`'s arms are as much a
             * place for those to meet as a branch's. `cfg.cpp`'s `cloneTerminator` had the same bug
             * and the fix is the same one - `makeSwitchEdges`, which is to `makeSwitch` what
             * `makeBranchEdges` is to `makeBranch`.
             */
            case Inst::Switch: {
                auto& written = (InstSwitch&)from;
                auto items = written.cases.pointer(base());
                auto count = written.cases.size();

                SmallArray<I64, 16> values;
                SmallArray<Block*, 16> targets;
                SmallArray<Size, 16> counts;
                SmallArray<Use, 16> operands;

                for(Size i = 0; i < count; i++) {
                    values.push(items[i].value);
                    targets.push(mapBlock(clone, items[i].edge.target));

                    SmallArray<Use, 8> own;
                    mapUses(clone, items[i].edge.arguments, own);

                    for(auto used: own) operands.push(used);
                    counts.push(own.size());
                }

                SmallArray<Use, 8> arguments;
                mapUses(clone, written.fallback.arguments, arguments);

                editor.replaceTerminator(into,
                    editor.makeSwitchEdges(at, mapUse(clone, written.selector), values.pointer(),
                                           targets.pointer(), counts.pointer(), operands.pointer(),
                                           count, *mapBlock(clone, written.fallback.target),
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

    // Whether a callee's parameter is only read before it is first written - see `inlineSite`'s
    // tier-1 rule. One block, so instruction order is execution order; any use but a load before
    // the first store and a store into it (of anything but itself) after is a refusal.
    bool storesThenNeverReads(Body& body, SsaPtr<Value> parameter) {
        if(reachableBlockCount(body) != 1) return false;

        auto entry = base()[body.entry];
        auto stored = false;
        auto safe = true;

        for(Size i = 0; i < entry->instructionCount() && safe; i++) {
            auto& inst = *base()[entry->instructionAt(base(), i)];
            forEachUse(base(), inst, [&](Use used) {
                if(used.value() != parameter) return;

                if(inst.kind == Inst::Store && ((InstStore&)inst).address.value() == parameter &&
                   ((InstStore&)inst).value.value() != parameter) {
                    stored = true;
                    return;
                }

                if(inst.kind == Inst::Load && !stored) return;
                safe = false;
            });
        }

        return safe;
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
     * The `fun.value` a call goes through, where **this body** is what built it - `analysis.h`'s
     * `funValueBehind`, over the memo of `soleContents` for the body as it is now.
     *
     * The frame slot step is the one that makes it worth having: `let f = (v) -> v * scale` puts
     * the pair in a slot, and without it every lambda a chain is written with stays an indirect
     * call. It was this pass's own `soleStoredValue` until J148, which read the slot's direct use
     * list and took every `load` for a read - so a borrow of the slot lent to `replaceFn(&f)` was a
     * read, and the call after it went to the function first stored (`FunSlotWrite.yana`).
     *
     * A capturing value in a slot that is dropped is refused - J145: the slot owned its value, an
     * owned capturing value consumed its environment, and a direct call would name an environment
     * that is already gone.
     */
    InstFunValue* funValueBehind(Value& target) {
        auto& slots = soleContentsOf(opt);
        auto dropped = false;

        auto made = ssa::funValueBehind(opt, slots, containedOf(opt), target, dropped);
        if(made && dropped && made->capturing) return nullptr;
        return made;
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

        return callDeclarationDirectly(opt, call, *made);
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
     * The same, where the callee reads the function it is handed in a block that is not **cold** -
     * J138. A link is copied to make the call through the value a direct one, and a call in a cold
     * arm is not worth making direct: `check`'s `@lazy note` is forced only on the failure path,
     * and treating `check` as a link copied the failure arm into every assertion of `test/base`.
     * Read from the callee's own `Block::cold`, which its last round left; a callee no round has
     * marked reads as hot, which is the answer this gave before.
     */
    bool handsOverHotFunValue(InstCall& call, mir::Function& callee) {
        if(!callee.ssa) return handsOverLocalFunValue(call);

        auto body = base()[callee.ssa];
        auto entry = base()[body->entry];
        if(entry->argumentCount() != call.arguments.size()) return handsOverLocalFunValue(call);

        for(Size i = 0; i < call.arguments.size(); i++) {
            auto used = call.arguments.get(base(), i);
            if(!used.isSet() || !funValueBehind(*base()[used.value()])) continue;

            if(readHot(*base()[entry->argumentAt(base(), i)])) return true;
        }

        return false;
    }

    // Whether a use of this value stands in a block that is not cold.
    bool readHot(Value& value) {
        for(auto userPointer: value.uses(base())) {
            auto user = base()[userPointer];
            if(!user->block || !base()[user->block]->cold) return true;
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
                if(!callee->environment && !handsOverHotFunValue(call, *callee)) continue;

                if(!inlineSite(function, call, true)) continue;

                spent += sizeOf(*callee);
                moved = true;
            }

            if(!moved) break;
            flattened = true;
        }

        return flattened;
    }

    bool inlineSite(mir::Function& caller, InstCall& call, bool flattening = false, bool once = false) {
        if(call.through != mir::CallThrough::Declaration || !call.callee) return false;
        if(decl()[call.callee]->teardown && eraseInertTeardown(opt, call)) return true;
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

        /*
         * A `@bits(n)` place lent to a `&` of what it refines - repr.md's tier 1 - is inlined only
         * where narrowing at each store is the call's answer. At a call the callee works on a
         * full-width temporary that is committed back masked when it returns (`lower`'s
         * `materializeNarrow`, JavaScript's one-slot box); spliced, its stores land in the field
         * itself and each narrows. The two agree unless the callee reads the parameter after
         * writing it: `x = x + 20; return x` over a `@bits(4)` field reads back 23 through a call
         * and 7 spliced, whenever forwarding does not carry the stored value to the read first
         * (`Packed.ReadBack.yana`'s loop, J149). `storesThenNeverReads` is that question asked
         * conservatively - one block, only loads before the first store and only stores after it -
         * which keeps `bump`, `tick` and `scale` inlined. The alternative is building the
         * temporary here - a frame slot, a load before the body and a commit after it - which is
         * more machinery than the construct has earned while declining is always sound.
         */
        for(Size i = 0; i < entry->argumentCount(); i++) {
            auto supplied = base()[call.arguments.get(base(), i).value()];
            auto declared = base()[entry->argumentAt(base(), i)];
            if(supplied->type() != declared->type() && mir::isRefinedInt(decl(), supplied->type()) &&
               mir::fillsParameter(decl(), supplied->type(), declared->type()) &&
               !storesThenNeverReads(*body, entry->argumentAt(base(), i))) {
                return false;
            }
        }

        // Whether the *caller's* block is rare - see `findColdBlocks`. A fact about the site rather
        // than about the callee, which is why it is read here and passed in.
        if(!worthInlining(call, flattening, inColdBlock(call.block), once)) return false;

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

        auto count = [&](DeclPtr<mir::Function> callee, bool isCall, bool cold = false) {
            if(!callee) return;

            auto entry = references.add(callee.offset);
            if(!entry.existed) *entry.value = References();

            if(!isCall) { entry.value->other++; return; }

            entry.value->calls++;
            if(cold) entry.value->coldCalls++;
        };

        Array<U32> order;

        // The caller this walk is currently inside, so that `findColdBlocks` reads the declaration
        // whose body it is being asked about - the `@cold` seeding is a fact about that declaration
        // rather than about the graph. Restored on the way out for the same reason `settle` restores
        // it: the position is the driver's, not this walk's.
        auto caller = opt.function;
        auto callerBody = opt.body;

        for(auto modulePointer: opt.program.modules) {
            auto module = decl()[modulePointer];

            for(auto pointer: module->functions.contents(decl())) {
                auto function = decl()[pointer];
                if(!function->reachable || !function->ssa) continue;

                auto body = base()[function->ssa];

                /*
                 * Which of this caller's blocks are cold, before its calls are counted.
                 *
                 * The same answer `runFunction` asks for the caller it is about to walk, asked here
                 * of *every* caller instead - which is what a rule about a callee's whole reference
                 * set needs and what a per-caller answer cannot give. It is recomputed there rather
                 * than reused, because a splice makes blocks and this count is the round's opening
                 * position; see `coldBlocks`.
                 */
                opt.function = function;
                opt.body = body;
                findColdBlocks(*body);

                reversePostorder(base(), *body, order);

                for(auto index: order) {
                    auto block = body->block(base(), BlockId(index));
                    auto cold = index < coldBlocks.size() && coldBlocks[index];

                    for(auto instPointer: block->instructions(base())) {
                        auto inst = base()[instPointer];

                        if(inst->kind == Inst::Call) {
                            auto& call = (InstCall&)*inst;
                            count(call.callee, call.through == mir::CallThrough::Declaration, cold);

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

        opt.function = caller;
        opt.body = callerBody;
    }

    bool runFunction(DeclPtr<mir::Function> pointer) {
        auto& function = *decl()[pointer];

        opt.function = &function;
        opt.body = base()[function.ssa];

        findColdBlocks(*opt.body);
        findOnceBlocks(function);

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
        pendingOnce.clear();

        for(U32 i = 0; i < opt.body->blocks.size(); i++) {
            auto block = opt.body->block(base(), BlockId(i));
            if(block->deleted) continue;

            auto once = i < onceBlocks.size() && onceBlocks[i];

            for(auto pointer: block->instructions(base())) {
                if(base()[pointer]->kind != Inst::Call) continue;

                pending.push(pointer);
                pendingOnce.push(once);
            }
        }

        auto inlined = false;

        for(Size i = 0; i < pending.size(); i++) {
            auto inst = base()[pending[i]];
            if(!inst->block) continue;

            inlined = inlineSite(function, (InstCall&)*inst, false, pendingOnce[i]) || inlined;
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

/*
 * `devirtualizeSite`'s rewrite, given the `fun.value` the call goes through - see the comment there
 * for the argument. A free function since J145, because `forwardMemory` answers the same question
 * for a *non-capturing* value it knows is in a slot (a field of a record literal, a slot written
 * more than once) and the rewrite is the same one: a non-capturing value's environment is nothing
 * the call site has to name, so where the `fun.value` stands does not matter.
 */
bool callDeclarationDirectly(OptContext& opt, InstCall& call, InstFunValue& madeValue) {
    if(call.through != mir::CallThrough::Value || !call.target.isSet()) return false;

    auto made = &madeValue;
    if(!made->callee) return false;

    auto callee = opt.decl[made->callee];

    /*
     * A generic compiled once is reached across the erased ABI, whose argument zero is a witness
     * table rather than an environment - so a value of one is a *thunk* over the pair and the
     * table it closes over is not written down here. `InstCall::environment` is what a direct
     * call to one would need and a `fun.value` does not carry it.
     */
    if(mir::isGeneric(opt.decl, callee->gen)) return false;

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
        auto& parameter = *opt.decl[callee->args.get(opt.decl, index)];
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

    for(auto used: call.arguments.contents(opt.base)) {
        Use supplied;
        if(!declared(position++, used, supplied)) return false;

        arguments.push(supplied);
    }

    auto result = call.results.size() ? opt.base[call.results.get(opt.base, 0)] : nullptr;

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
        if(result->loan) editor.setDependency(*answered, *opt.base[result->loan]);

        editor.replaceAllUses(*result, *answered);
    }

    editor.eraseInst(call);
    return true;
}

void inlineCalls(OptContext& opt) {
    auto level = opt.context.settings.inlining;
    auto js = isJsMode(opt.context.settings);
    auto weighOnce = !js && (level == InlineLevel::Balanced || level == InlineLevel::Speed);
    auto onceLevel = weighOnce ? InlineLevel::Size : level;

    Inliner inliner { opt, budgetFor(level, js), budgetFor(onceLevel, js), weighOnce };

    /*
     * **Not** an early-out at `InlineLevel::None` - J49.
     *
     * It was one, on the reading that a zero ceiling leaves nothing qualifying. That is true of
     * everything weighed and false of the one thing that is not: `@inline(always)` is a directive,
     * the conversion's graft applies no size test to it, and a call the graft declined - eight
     * expansions deep, or through a value - has only this pass left to honour it. Returning here
     * made the attribute mean nothing at `-inline none`, which is a supported way to build and is
     * exactly the "cost depends on a flag" that the shipping stage's `resolve/inline.h` argued a
     * directive may not have.
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

        /*
         * **Teardowns first**, then everything else - J137.
         *
         * The order within a round is the module order, which is neither bottom-up nor top-down, and
         * for most callees it does not matter: a chain collapses one level per round. It matters for
         * a teardown because the site that copies one decides once. `Array.reclaim` copied into
         * `main` before `releaseRun` had been copied into it brought the call with it, and by the
         * next round `freeHeap` had been copied into `releaseRun` and the pair was over the budget -
         * so `main` kept a call handing over a `Run` built in memory for a flag `placeRuns` then
         * proved was zero. A teardown's own callees are the release primitives and nothing calls
         * back up, so visiting the teardowns first is bottom-up exactly where it is needed.
         */
        for(auto pass = 0; pass < 2; pass++) {
            for(auto modulePointer: opt.program.modules) {
                auto module = opt.decl[modulePointer];

                for(auto pointer: module->functions.contents(opt.decl)) {
                    auto function = opt.decl[pointer];

                    // A body nothing can reach is not one to copy anything into: it is not emitted,
                    // and its own call sites have just stopped being counted.
                    if(!function->reachable || !function->ssa) continue;
                    if(function->teardown != (pass == 0)) continue;

                    inlined = inliner.runFunction(pointer) || inlined;
                }
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
