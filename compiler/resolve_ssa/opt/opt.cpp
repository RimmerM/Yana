#include "pass.h"
#include "../verify.h"

/*
 * The driver: which passes run, over which bodies, and how many times.
 *
 * What each pass decides lives in the file that decides it. The loop is a fixed point rather than a
 * sequence because the passes feed each other - folding an operand leaves a value unread, removing
 * that one leaves its operands unread in turn, and a branch on a folded condition is a block that
 * goes away.
 *
 * The IR surgery is not here and never was: `Editor` in `resolve_ssa/build.h` is the only thing that
 * writes a body, and it is reached from every pass as `opt.ir()`.
 */

namespace ssa {

namespace {

/*
 * A cap on the fixed point rather than a trusted termination proof.
 *
 * Every rewrite here strictly reduces something - an operand becomes a constant, an instruction goes
 * away, a block goes away - so the loop terminates on its own; the cap is what turns a future pass
 * that oscillates into a slow compile rather than a hang.
 *
 * Four, where the shipping stage's is eight. `simplify` and `cleanCfg` each collapse a fixed point
 * that driver spends rounds on - one worklist over uses and one over blocks - so the rounds here
 * exist for the passes that genuinely see a different body than the round before. Raise it if a
 * fixture is *measured* to need it.
 */
constexpr Size kMaxRounds = 4;

} // namespace

/*
 * One pass, and the IR it left behind - see `resolve_ssa/verify.h`.
 *
 * A pass is the smallest thing that can be blamed, so this is the checkpoint the verifier exists
 * for. The tier's README already says the verifier "is meant to run after every ownership-sensitive
 * pass in an assertion build"; this is that sentence made true. Running it here also gives the
 * "before" of every pass for free, since the check after one is the check before the next - and the
 * round below opens with one, which is the "before" of the first pass of the first round.
 *
 * The verifier reports each rule it found broken and says nothing about who broke it, so the line
 * this adds is the attribution: the function, and what had just run over it.
 *
 * Confined to assertion builds by the same switch `assertTrue` and the shipping stage's `verifyIr`
 * use, because this is one full walk of a body per pass per round. Declared in `pass.h` rather than
 * kept here, because the inliner rewrites a body outside the loop below and owes the same line.
 */
#if defined(_DEBUG) || defined(DEBUG)

void checkpoint(OptContext& opt, StringView where) {
    if(verifyBody(opt.context, opt.program, opt.base, *opt.function)) return;

    opt.context.diagnostics.error("internal: the SSA verifier rejected %@ %@"_v,
                                  opt.function->source, opt.context.findName(opt.function->name),
                                  where);
}

#endif

// A pass and the checkpoint behind it, so that the list below reads as the list and the attribution
// is not a line a new row can be added without.
#define runPass(pass) \
    pass(opt); \
    checkpoint(opt, "after " #pass ""_v)

void optimizeBody(OptContext& opt) {
    // What the conversion left, before this stage touches it. There is nothing special about this
    // checkpoint - every pass below maintains the same invariants - so it is simply the first of
    // them, and a failure here names the conversion rather than a pass.
    checkpoint(opt, "before optimizing"_v);

    for(Size round = 0; round < kMaxRounds; round++) {
        opt.changed = false;

        /*
         * First, and phase 10's note is the whole argument: the store-and-load traffic around every
         * local is what the seven passes below cannot see through, and four of them measured their
         * own answers away against it before this existed. `range` fired zero times, `convertSelects`
         * needed a `load` exception, `threadBranches` missed `let ok = ...` in front of `if ok`, and
         * the inliner counted a store and a load per parameter towards a budget of twenty.
         *
         * In front of `simplify` rather than behind it, because what this produces is operands:
         * a constant written into a local and read back below a join arrives at the folder as one
         * value only once this has run.
         */
        runPass(promote);

        /*
         * Then the placement, immediately behind `promote` and in front of everything that spends
         * it.
         *
         * Behind, because it asks `Contained` of every slot the buffer is written into and the
         * store-and-load traffic the conversion leaves would have made half of them escape. In
         * front, because what it produces is a *constant* - a descriptor whose flag says the frame
         * placed these bytes - and the folder, the forwarder and the graph cleanup below are what
         * turn that into a teardown that does nothing and a refusal arm that is not there. Placing
         * it at the end of the round instead left a body one round short: the request arrives by
         * inlining, so the round that sees it is often the last one there is.
         *
         * It answers nothing in round zero, where the count is still a load out of a slot, and that
         * costs nothing: `settle` restarts this loop on a caller the moment a body is spliced into
         * it, so the round after the arrival is the round that places.
         */
        runPass(placeRuns);

        /*
         * Then the folds, and README.md's section B is why: everything below wants folded operands,
         * and this is the one pass that inserts no instruction into a list it is walking - its queue
         * is over instructions rather than positions, so a constant made in front of the instruction
         * being folded is not a cursor the walk has to repair.
         */
        runPass(simplify);

        /*
         * Then the two the folds have just made answerable, in that order.
         *
         * `cse` is behind `simplify` because it compares operands as SSA values: two spellings of
         * one expression are one computation only once their operands have been unified, and the
         * folder is what unifies them. `hoist` is behind `cse` for the same reason one step out - a
         * value the loop body spelled twice leaves the loop once rather than not at all.
         *
         * Both are in front of the storage passes and that is the one ordering worth arguing.
         * `forwardMemory` carries a fact across the instructions of one block, so a read this has
         * hoisted into a preheader is one it answers there; running it first would leave the same
         * read in the loop with a block-local fact attached to it and nothing to move.
         */
        runPass(cse);
        runPass(hoist);

        /*
         * Then storage, and README.md's section B is the order: the store/load traffic the
         * conversion emits around every local is what hides a constant from every pass below, and
         * removing an allocation is worth asking only *after* the forwarding that makes its reads go
         * away. The two are separate passes because the second is an ownership edit - it deletes a
         * `Drop` with an `Alloc` - and owes a verifier run of its own.
         */
        runPass(forwardMemory);
        runPass(removeDeadStorage);

        /*
         * Then the loops, immediately in front of the CFG cleanup, because what `deadLoops`
         * produces is a constant exit test and nothing else: it deletes no block, and the pass that
         * owns the region rule is the one below it.
         */
        runPass(deadLoops);

        /*
         * And the graph, after the folds, because most constant conditions are made rather than
         * written: a comparison the folder answered is what turns a branch into a jump, and the arm
         * that stops being reachable is what leaves a join with one way in.
         *
         * `convertSelects` is **between** the two, which is README.md's section B rows 6 and 7. The
         * fold has first claim, because a deleted arm beats a select over both; and threading runs
         * behind if-conversion, because sending a predecessor past a join duplicates the decision
         * onto an edge and leaves no diamond to convert - see the note in `select.cpp`.
         */
        /*
         * And divergence, immediately in front of the graph cleanup that spends it: what this leaves
         * is a block ending in `unreachable` and a join with one predecessor fewer, and both of
         * those are `cleanCfg`'s to fold. Above it rather than below, so the fold happens in the
         * same round the edge was cut in rather than in the next one.
         */
        runPass(endNonReturningBlocks);

        runPass(cleanCfg);
        runPass(convertSelects);

        /*
         * And the two that make block arguments constant, in that order.
         *
         * `decideBranchConditions` rewrites an operand and `threadBranches` moves an edge, so the
         * first is what gives the second something to move - and `removeTrivialArguments`, which
         * both `promote` and `cleanCfg` call, is what spends the result on the next round.
         */
        runPass(decideBranchConditions);
        runPass(threadBranches);

        // And that is the whole list. What is *not* here is `range`: phase 8 built the branch-implied
        // facts, measured them over the corpus and did not keep them, and phase 11 rebuilt them on
        // promoted IR and got the same answer - README.md's two notes say what the numbers were and
        // why they are structural. `inlineCalls` and `markProgramReachable` are not here either, and that is the other half of the arrangement: they are program-wide and run
        // once, in front of this loop - see `optimizeProgram`.

        if(!opt.changed) break;
    }
}

/*
 * One pass over every body the backend will read - the loop `optimizeProgram` runs below, lifted so
 * that a program-wide pass may be run once in front of the inliner without a second copy of the
 * filter.
 *
 * The filter is the lowering's: a declaration with no `ssa` handle is a signature or a body the
 * conversion declined, and an unreachable one is a body nothing emits.
 */
template<class F>
static void eachOptimizedBody(OptContext& opt, mir::Program& program, F&& pass) {
    for(auto modulePointer: program.modules) {
        auto module = opt.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(opt.decl)) {
            auto function = opt.decl[functionPointer];
            if(!function->reachable || !function->ssa) continue;

            opt.function = function;
            opt.body = opt.base[function->ssa];
            pass(opt);
        }
    }
}

bool optimizeProgram(Context& context, mir::Program& program, Converted& converted) {
    OptContext opt { context, program, converted, *program.decl, *converted.arena };

    /*
     * The one pass above the `-no-ir-opt` gate, and the shipping stage's reason for putting it there
     * is the whole of why it is here too: `-no-checks` has to stay **free** rather than cheap, and
     * the two settings are independent. An ordinary pass would leave a build with the optimizer off
     * computing a length, a comparison and a branch at every subscript and then keeping all three.
     *
     * What it removes is not a check this stage inserted - it inserts none - but the library's own,
     * which `lib/Core/Array.yana`'s `Index` instance writes and `@inline(always)` grafts into
     * every caller. `opt/check.cpp` is where that shift in the switch's meaning is argued.
     */
    auto discharged = dischargeChecks(opt);

    /*
     * And the gate itself, as one early return with nothing else above it.
     *
     * `Pipeline::convert` ends with an unconditional `verifyProgram`, in every build, so a program
     * no pass has run over is that same program and owes nothing here. One that the discharge did
     * rewrite is not, which is what the answer above is for.
     */
    if(!context.settings.optimizeIr) {
        return discharged ? verifyProgram(context, program, converted) : true;
    }

    /*
     * The program-wide passes first, in README.md's section B order.
     *
     * `inlineCalls` is in front of the per-function loop because what a copy buys is not the call it
     * removes: it is that `simplify` can then fold the callee against the literals the site passed,
     * that `forwardMemory` can answer a parameter's loads from the store that filled it, and that
     * `cleanCfg` can delete the arm a folded condition stranded. Running it after the loop would
     * leave every one of those for a second run that does not happen.
     *
     * `markProgramReachable` is inside it rather than beside it - it runs at the top of each of the
     * inliner's rounds and once at the end - because the answer it corrects is one the inliner's own
     * threshold reads. What the loop below then gets is a `reachable` set that says what the program
     * holds now, so a body whose last call was spliced away is neither optimized nor lowered.
     */
    /*
     * Divergence, once over every body, **in front of the inliner** - and the order is the whole of
     * why it is here rather than only in the loop below.
     *
     * `endNonReturningBlocks` reads the *callee's* summary at a `Call`, and the inliner's whole job
     * is to remove that call. `checkFailed` is 20 instructions once its `abortProcess` is spliced
     * in, and what came out the other side was a bounds check whose failure arm ended in a `jump`
     * back into the code that runs: the fact was true right up to the transformation that erased its
     * evidence, and nothing below could infer it again - a `syscall` is not divergence, it is a
     * number this compiler declines to know.
     *
     * What it cost was not the arm. A live edge out of the failure arm makes the block below every
     * check a *join*, which ends every block-local fact in this directory: two checks of one
     * container read its length twice, `forwardMemory` stops at each one, and the abort arms are
     * ordinary blocks the lowering's `mergeExitBlocks` may not collapse because they have a
     * successor.
     *
     * The alternative was for the splice to carry the fact - a diverging callee's join is
     * unreachable, and the inliner could say so. It is a rule about the *caller's* block shape
     * stated inside a rewrite that otherwise has no opinion about one; settling divergence before
     * the pass that spends it is one statement instead, and `inlineCalls` then clones bodies whose
     * dead arms are already dead.
     */
    /*
     * The calling convention first, and above the inliner for the reason `opt/arg.cpp` gives: a
     * signature this rewrites is rewritten once in the callee rather than once per copy of it, and
     * what it leaves behind - a record rebuilt in the callee's entry, taken apart at the caller - is
     * exactly what `promote`, `forwardMemory` and `removeDeadStorage` remove in the loop below.
     */
    flattenArguments(opt);

    eachOptimizedBody(opt, program, [](OptContext& body) {
        checkpoint(body, "after flattenArguments"_v);
        endNonReturningBlocks(body);
        checkpoint(body, "after endNonReturningBlocks"_v);
    });

    inlineCalls(opt);

    for(auto modulePointer: program.modules) {
        auto module = opt.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(opt.decl)) {
            auto function = opt.decl[functionPointer];

            /*
             * Every body the backend will read, and nothing else.
             *
             * The same filter the lowering applies: a declaration with no `ssa` handle is a
             * signature, an intrinsic or a body the conversion declined, and an unreachable one is
             * a body nothing emits. Core and the prelude are resolved on every compilation and a
             * program reaches a few dozen of them, so this is most of what the walk would otherwise
             * spend its time on.
             *
             * `reachable` is the MIR stage's answer rather than a fresh one. Once the inliner
             * arrives that answer goes stale in the safe direction for *skipping* work and the
             * unsafe direction for a threshold, which is why phase 9 brings `reach` with it.
             */
            if(!function->reachable || !function->ssa) continue;

            opt.function = function;
            opt.body = opt.base[function->ssa];
            optimizeBody(opt);
        }
    }

    /*
     * And the globals, once, with every body in its final shape.
     *
     * The folder answers an immutable global's read with the number it holds, and what was left
     * behind was the storage: ten of `lib/`'s constants were emitted as `.data` in every program
     * that allocated and each was referenced exactly once, by its own definition. It is below the
     * loop rather than inside the inliner's rounds because the reads it is counting are the ones
     * that *survived* - see `markReachableGlobals`, where the two reachability questions are
     * separated.
     */
    markReachableGlobals(opt);

    /*
     * And one unconditional walk of the whole program on the way out, in every build.
     *
     * The per-pass checkpoint above is an assertion build's, and it is what makes a failure
     * attributable; this one is what stands between the optimizer and the backend. Everything below
     * reads this IR as a promise - the lowering assumes a value is defined before it is read and
     * that an owner is consumed once - so a release build that skipped the check would be one where
     * a pass bug becomes a miscompile rather than a diagnostic.
     */
    return verifyProgram(context, program, converted);
}

} // namespace ssa
