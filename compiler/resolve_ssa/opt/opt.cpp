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
         * First, and README.md's section B is why: everything below wants folded operands, and this
         * is the one pass that inserts no instruction into a list it is walking - its queue is over
         * instructions rather than positions, so a constant made in front of the instruction being
         * folded is not a cursor the walk has to repair.
         *
         * Phases 5 to 9 fill in the rest of the list, in that section's order.
         */
        runPass(simplify);

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
        runPass(cleanCfg);
        runPass(convertSelects);
        runPass(threadBranches);

        // And that is the whole list. What is *not* here is `range`: phase 8 built the branch-implied
        // facts, measured them over the corpus and did not keep them - README.md's note says what the
        // numbers were and why they are structural. `inlineCalls` and `markProgramReachable` are not
        // here either, and that is the other half of the arrangement: they are program-wide and run
        // once, in front of this loop - see `optimizeProgram`.

        if(!opt.changed) break;
    }
}

bool optimizeProgram(Context& context, mir::Program& program, Converted& converted) {
    /*
     * The `-no-ir-opt` gate, as one early return with nothing above it.
     *
     * The shipping stage has `dischargeChecks` above its own gate, because `-no-checks` has to stay
     * free rather than cheap and the two settings are independent. Nothing here answers to that:
     * this stage inserts no check - `prelude/Core/Array.yana`'s `Index` instance writes its own and
     * says so - so there is nothing this side of the return.
     *
     * Nothing is verified on the way out of this branch either, and that is not an omission:
     * `Pipeline::convert` ends with an unconditional `verifyProgram`, in every build, and a program
     * no pass has run over is that same program.
     */
    if(!context.settings.optimizeIr) return true;

    OptContext opt { context, program, converted, *program.decl, *converted.arena };

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
