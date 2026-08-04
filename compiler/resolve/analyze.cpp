#include "analyze_pass.h"

/*
 * The driver.
 *
 * Which passes run, in which order, and how many times. Every fact they compute lives in the file
 * that computes it; what is here is the two things that are nobody else's: the order each pass needs
 * the one before it in, and the fixpoint that settles the summaries the interprocedural half reads.
 */

/*
 * The program's buffers, made on first use and kept until it is destroyed.
 *
 * On the program rather than on the analysis because the analysis is the thing there are a thousand
 * of: a compilation runs these passes twice over every function of Core, Native and Collections
 * before it reaches one the source wrote, and what they need is the same shape every time. Sized to
 * the largest function seen so far and never shrunk - see AnalysisScratch.
 */
static AnalysisScratch& scratchFor(Program& program) {
    if(!program.analysisScratch) program.analysisScratch = new AnalysisScratch();
    return *program.analysisScratch;
}

void destroyAnalysisScratch(AnalysisScratch* scratch) {
    delete scratch;
}

Analysis::Analysis(Module& module, Function& function):
    module(module), context(module.context), global(*module.types), local(*module.arena),
    function(function), scratch(scratchFor(module.program)),
    liveIn(scratch.liveIn), liveOut(scratch.liveOut), values(scratch.values),
    contents(scratch.contents), outlives(scratch.outlives), escaped(scratch.escaped),
    transferred(scratch.transferred), releasesStorage(scratch.releasesStorage),
    stateBefore(scratch.stateBefore), order(scratch.order), blockRanges(scratch.blockRanges),
    effects(scratch.effects), tracked(scratch.tracked), demand(scratch.demand),
    indexOf(scratch.indexOf) {

    // Emptied here rather than by whichever pass fills each one, so that "this run starts with
    // nothing" is one statement in one place - see AnalysisScratch for why they are not fresh.
    order.clear();
    blockRanges.clear();
    tracked.clear();
    demand.clear();
    indexOf.reset();
}

/*
 * One of the pool's provenances, for the length of one expression.
 *
 * The depth is the whole of the bookkeeping: a borrow takes the set at the current depth and
 * releasing it puts the depth back, so nesting works and nothing is ever handed to two holders at
 * once. The pool grows to the deepest nesting any function reached and stops there.
 */
ScratchProvenance::ScratchProvenance(Analysis& analysis): scratch(analysis.scratch) {
    if(scratch.borrowDepth == scratch.borrowed.size()) scratch.borrowed.push(new Provenance());

    set = scratch.borrowed[scratch.borrowDepth++];
    set->reset(analysis.localCount);
}

ScratchProvenance::~ScratchProvenance() {
    scratch.borrowDepth--;
}

/*
 * One function, once.
 *
 * `reporting` is what separates the fixpoint's silent rounds from the one round whose diagnostics
 * are the program's, and `rewrite` whether this run is allowed to insert drops and choose storage.
 * Everything before those two switches is the same work either way: the facts do not depend on
 * which round computed them, only on the summaries that were available when they did.
 */
static bool analyzeFunction(Module& module, Function& function, OwnershipResult& result,
                            bool reporting, bool rewrite, bool* summaryChanged) {
    Analysis analysis(module, function);
    analysis.localCount = function.localCount();
    analysis.reporting = reporting;
    analysis.rewriting = rewrite;

    if(function.blocks.isEmpty()) return true;

    numberFunction(analysis);
    computeEffects(analysis);

    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = function.localAt(analysis.local, U32(l));
        auto ownership = ownershipOf(module, slot.type);

        /*
         * Which slots this frame is responsible for releasing.
         *
         * A borrowed parameter - the default convention, or `&` - refers to storage the caller
         * owns and keeps; dropping it here would release something the caller still holds. A `->`
         * parameter is the opposite: the caller handed ownership over and recorded the handover as
         * an InstMove, so this frame is the one that owes it a drop.
         *
         * The exception is a function that *is* the drop, or the sink. `Drop::drop` receives the
         * value in order to release it, and dropping its own parameter at the end would call
         * itself forever; `Sink::sink` empties its source into the destination, so what is left is
         * not something to release either. Both are the two places in the language where a `->`
         * parameter's disposal is the body's own business.
         *
         * Derived teardown glue is the third and is asked by flag rather than by class, because it
         * is anonymous and has no `instanceOf` to be recognized by - see Function::disposer, which
         * records why it needs one now and did not before.
         */
        auto parameter = isParameterSlot(analysis, l);
        auto disposer = function.disposer ||
                        function.instanceOf == module.coreClasses.drop ||
                        function.instanceOf == module.coreClasses.reclaim ||
                        function.instanceOf == module.coreClasses.sink;

        auto owned = parameter
            ? (slot.convention == ast::BindType::Sink && !disposer)
            : !slot.borrowed;

        // A closure's environment is allocated here and owned by the function value built out of
        // it, so this frame neither drops it nor hands its storage back on its own account - the
        // value's teardown does both. It may still be *reached* by name where that teardown turns
        // out to be one this frame can see through; see closureTeardown, which is what keeps the
        // two from both happening. See Local::closureEnv.
        if(slot.closureEnv) owned = false;

        // By name, for the reason Function::addLocal gives: this is the other struct in the
        // ownership passes that gets fields added to it, and a positional list of four out of eight
        // is exactly the shape that goes wrong quietly when a fifth is inserted in front.
        analysis.tracked.push(TrackedLocal {
            .type = slot.type,
            .name = slot.name,
            .owned = owned,
            .droppable = ownership.needsTeardown(),
        });
    }

    /*
     * One row per instruction, every row already the width of the frame.
     *
     * It was `reset(instructionCount, 0, ...)`, which leaves every row *empty* until the ownership
     * walk fills it - and that walk only visits blocks it can reach. An unreachable block's
     * instructions therefore kept an empty row, and every consumer indexes a row by local: the
     * first use in one asserted `i < count` against a row of zero. A body with an unreachable block
     * in it is what a *rejected* declaration leaves behind - `Circle(c)` matched against a pivot
     * that no longer has a `Circle`, which is one keystroke inside a `data` line - so this is
     * reachable from the editor on any keystroke and from the driver on any program that already
     * failed.
     *
     * Filled rather than skipped, because `Uninitialized` is what an unreachable instruction's
     * state *is*: nothing there owns anything, so no use-after-move is reported for code that never
     * runs and no drop is inserted into it. The reachable rows are replaced wholesale by
     * `copyInto`, so pre-filling costs them nothing.
     */
    analysis.stateBefore.reset(analysis.instructionCount, analysis.localCount, OwnState::Uninitialized);

    computeLiveness(analysis);
    computeOwnership(analysis);

    // The interprocedural half, in the order each part needs the one before it: which storage every
    // value refers to, what has to outlive the frame, what each root's representation must do, and
    // finally what all of that says to a caller.
    computeProvenance(analysis);
    computeOutliving(analysis);
    computeDemand(analysis);

    auto changed = deriveSummary(analysis);
    if(summaryChanged) *summaryChanged = changed;

    // A silent round exists to move the summary and nothing else. Checking here as well would be
    // harmless but wasted, and rewriting would apply a decision the fixpoint has not settled yet.
    if(!rewrite) return true;

    checkMoves(analysis);
    checkBorrows(analysis);
    checkReturnRoots(analysis);
    checkMaterializedBorrows(analysis);
    checkEscapingViews(analysis);
    checkClosureEnvironments(analysis);

    replaceContents(result.locals, analysis.tracked);
    for(Size l = 0; l < analysis.localCount; l++) {
        result.locals[l].requirements = analysis.demand[l];
        result.locals[l].escapes = analysis.escaped[l] != 0;
    }

    selectStorage(analysis, result);
    buildRanges(analysis, result);

    // Nothing is rewritten once something has been reported: the IR the diagnostics were derived
    // from is the one worth printing, and inserting drops into a body already known to be wrong
    // produces a second round of diagnostics about the first round's mistakes.
    if(!analysis.ok) return false;

    // A generic body is checked and then left alone. Its type variables classify conservatively -
    // Design.md requires an unconstrained parameter to be treated as owning something - so drops
    // derived here would be drops of a type nothing knows the shape of. What reaches the backend is
    // this function's specializations, and each of those is an ordinary function that gets its own.
    if(function.gen) return true;

    insertDrops(analysis);
    return analysis.ok;
}

bool runOwnership(Module& module, Function& function, OwnershipResult& result) {
    return analyzeFunction(module, function, result, true, true, nullptr);
}

// Which functions the passes run over. A signature has no body, an intrinsic is generated at each
// call site rather than being one function, and a generic body is checked but never given drops -
// what reaches the backend is its specializations, and those are ordinary functions that get their
// own drops here. Checking the generic body anyway is what puts a use-after-move diagnostic on the
// function that has the bug instead of once per instantiation.
static bool ownershipApplies(Function& function) {
    return !function.signature && !function.intrinsic && function.blocks.isNotEmpty();
}

/*
 * The whole program, in two phases.
 *
 * A summary is a statement about a function that its callers read, so a caller cannot be analyzed
 * before its callees - and with recursion there is no order in which that is true. So the summaries
 * are settled first, silently, and only then is every function analyzed once more for real. Every
 * fact involved is a "may" fact climbing from empty, so the silent phase is optimistic and only ever
 * adds; the reporting phase therefore sees every summary at its final value, and what it reports is
 * what the program means.
 *
 * What settles them is a worklist over the call graph rather than rounds over the program. The two
 * reach the same fixpoint - each is the ordinary way to solve the same monotone system - and the
 * difference is what gets recomputed: a round re-runs every function's intraprocedural analysis
 * whether or not anything it reads has moved, which for a program of any size is most of the work
 * spent re-deriving facts that did not change. A function is instead re-analyzed exactly when a
 * callee's summary moved.
 *
 * Still without keeping any function's facts alive between visits, which is the resource that was
 * never worth trading: what is cached is the *edges*, one list of callees per function, and a
 * function that is not woken costs nothing at all.
 */

// One function the passes run over, with the module its diagnostics and instances belong to.
struct SummaryWork {
    Module* module;
    ModulePtr<Function> function;
};

/*
 * Which functions one body's summary depends on.
 *
 * Direct callees, and nothing else: summaryOf() is the only way an intraprocedural pass reads
 * another function, and every one of its four call sites hands it an InstCall's callee. A call
 * through a function value reads the *signature*, which is a declaration rather than a derived fact
 * and never moves, and an InstGenCall has no callee to summarize until it is specialized - which is
 * why both are conservative at the call site instead of being edges here.
 */
static void collectCallees(ModuleBase base, Function& function, SmallArray<ModulePtr<Function>, 16>& target) {
    for(auto blockPointer: function.blocks.contents(base)) {
        for(auto instructionPointer: base[blockPointer]->instructions.contents(base)) {
            auto& instruction = *base[instructionPointer];
            if(instruction.kind != Value::Call) continue;

            auto callee = ((InstCall&)instruction).callee;
            if(!callee) continue;

            auto seen = false;
            for(auto existing: target) seen = seen || existing == callee;
            if(!seen) target.push(callee);
        }
    }
}

// Runs the silent phase to a fixpoint.
static void settleSummaries(Program& program) {
    auto base = *program.arena;

    Array<SummaryWork> work;
    for(auto module: program.modules) {
        for(Size i = 0; i < module->functionOrder.size(); i++) {
            auto pointer = module->functionOrder.get(base, i);
            if(ownershipApplies(*base[pointer])) work.push(SummaryWork { module, pointer });
        }
    }

    // The reverse graph, which is what a worklist walks: who has to be woken when this function's
    // summary moves. Keyed by the callee's arena offset, holding indices into `work`.
    // The rows are inline: most functions are called from a handful of places, and a row is one
    // allocation each otherwise. A SmallArray is safe as a map value here because its inline
    // storage is not pointed at from inside itself - `pointer()` computes the answer from whether
    // the heap buffer exists - so the map relocating a row on rehash relocates it correctly.
    HashMap<U32, SmallArray<Size, 8>> callers;

    for(Size i = 0; i < work.size(); i++) {
        // Inline, and rebuilt per function: what a function calls is a handful of names, and this
        // loop runs once for every function in the program.
        SmallArray<ModulePtr<Function>, 16> callees;
        collectCallees(base, *base[work[i].function], callees);

        for(auto callee: callees) {
            // add() hands back uninitialized storage for a key that was not there, so the list is
            // constructed into it rather than assigned - the same reason the results map below is.
            auto entry = callers.add(U32(callee));
            if(!entry.existed) new (entry.value) SmallArray<Size, 8>();

            entry.value->push(i);
        }
    }

    /*
     * Seeded with everything, because a first visit has to happen for every function anyway: until
     * one has run, a function's summary is not `ready` and every caller reads the conservative
     * answer for it.
     *
     * Pushed in reverse so that popping walks declaration order, which for this compiler is roughly
     * callees before callers - Core is analyzed before the module that uses it - and a first sweep
     * in that order is the one that wakes the fewest callers a second time.
     */
    Array<Size> pending;
    IndexSet queued;
    queued.reset(work.size());
    queued.fill();

    for(Size i = work.size(); i > 0; i--) pending.push(i - 1);

    /*
     * The bound, which is a guard rather than the reason this terminates.
     *
     * It terminates because every fact is monotone and the lattice is finite: a visit that changes
     * nothing wakes nobody. The cap is the same statement the round-based version made - no more
     * total visits than rounds-times-functions would have been - and it exists so that a rule added
     * later that is not monotone turns into a wrong answer rather than into a hang.
     */
    auto budget = work.size() * (work.size() + 2) + 8;
    Size visits = 0;

    while(pending.isNotEmpty() && visits < budget) {
        auto index = pending.pop().unwrap();
        queued.set(index, false);

        auto& item = work[index];
        OwnershipResult discarded;
        auto moved = false;

        analyzeFunction(*item.module, *base[item.function], discarded, false, false, &moved);
        visits++;

        if(!moved) continue;

        auto woken = callers.get(U32(item.function));
        if(!woken) continue;

        for(auto caller: woken.unwrap()) {
            if(queued[caller]) continue;

            queued.set(caller, true);
            pending.push(caller);
        }
    }
}

bool runProgramOwnership(Program& program) {
    auto base = *program.arena;
    auto success = true;

    if(!program.ownership) program.ownership = Ptr<OwnershipResults>(new OwnershipResults());

    // A signature has no body to summarize, so it says nothing rather than saying the optimistic
    // thing: a class method's implementation is chosen per instance, and a caller that assumed one
    // did not mutate its argument would be assuming it of every instance there will ever be.
    for(auto module: program.modules) {
        for(auto pointer: module->functionOrder.contents(base)) {
            auto function = base[pointer];
            if(ownershipApplies(*function)) continue;

            function->summary.opaque = true;
            function->summary.ready = true;
        }
    }

    settleSummaries(program);

    /*
     * And once more for real, over every function including the ones the silent phase never had to
     * wake twice: what a body reports and what drops it gets are decided against settled summaries.
     *
     * Analyzing a body *generates* functions - a teardown's glue, and the specialization of an
     * authored instance method it calls - and they land in the module the instance came from, which
     * is usually not the module being walked. Collections' `Reclaim(Array(a))` specialized at the
     * root module's element type is the case that matters: appended to Collections, which this walk
     * had already finished, so it went out with no drops in it and an array of elements with a
     * teardown released the run and leaked the elements.
     *
     * So the sweep repeats until no module grew. Each round is over what the previous one added, and
     * the number of rounds is the depth of the teardown graph rather than anything about the program
     * - a container of containers is two.
     */
    Array<Size> analyzed;
    for(Size i = 0; i < program.modules.size(); i++) analyzed.push(0);

    for(auto growing = true; growing;) {
        growing = false;

        for(Size m = 0; m < program.modules.size(); m++) {
            auto module = program.modules[m];

            // By index, because the list grows underneath this loop as well - the same reason
            // resolveModuleBodies does it.
            for(Size i = analyzed[m]; i < module->functionOrder.size(); i++) {
                auto pointer = module->functionOrder.get(base, i);
                auto function = base[pointer];

                if(function->instanceOf == program.coreClasses.reclaim) {
                    success = checkReclaimShape(*module, *function) && success;
                }

                if(!ownershipApplies(*function)) continue;

                OwnershipResult result;
                auto ok = analyzeFunction(*module, *function, result, true, true, nullptr);
                success = success && ok;

                // add() hands back uninitialized storage, so the result is constructed into it
                // rather than assigned - assigning would run the destructor of whatever the slot
                // happened to contain, which for a struct of Arrays means freeing garbage pointers.
                if(ok && !function->gen) {
                    new (program.ownership->functions.add(U32(pointer)).value) OwnershipResult(::move(result));
                }
            }

            if(analyzed[m] != module->functionOrder.size()) growing = true;
            analyzed[m] = module->functionOrder.size();
        }
    }

    return success;
}


/*
 * ---------------------------------------------------------------------------------------------
 * What this pass does not do yet.
 *
 * Everything below is a deliberate omission rather than an oversight, and each is conservative in
 * the same direction: the analysis either rejects a program it could have accepted, or drops later
 * than it had to, or gives a value more storage than it needed. Nothing here can make it accept a
 * program it should reject, which is the property worth preserving while the rest is filled in.
 *
 * **Drop flags.** A value moved out of on only some paths reaching its last use needs a runtime bit
 * saying whether the slot still owns anything. The bit, the block split around the conditional
 * drop, and InstDrop::flag are all designed for; what is here reports instead of emitting them.
 * This is the largest single item and the one an ordinary program hits first - `if c: consume(x)`
 * is enough.
 *
 * **Partial moves.** Moving one field out of an aggregate leaves the slot half-owned. checkMoves()
 * rejects it, because representing it means a drop flag per field and a drop that runs over a
 * subset of members - the same machinery drop flags need, one level further in.
 *
 * **Two-phase borrows.** `f(&x, g(x))` evaluates `g(x)` while the borrow of `x` for the first
 * argument is already live, which is rejected here and accepted by Rust through a reservation
 * phase. The resolver happens to evaluate arguments before creating the borrow, so the common
 * shapes do not hit it, but the rule is not stated anywhere and should be.
 *
 * **Per-field granularity for liveness, ownership and demand.** All three are tracked per local, so
 * borrowing `x.a` keeps all of `x` alive and writing `x.a` makes all of `x` writable. Conflict
 * *detection* is per place and does distinguish `x.a` from `x.b`; it is only the extent and the
 * demand that are coarse. Containment in the provenance analysis is field-insensitive for the same
 * reason and with the same effect.
 *
 * **Demand does not follow a move.** Design.md says an ownership root keeps its demand across a
 * move, and here a `->` binding starts a new local with a demand of its own. What that costs is
 * precision in one direction only - a value moved and then mutated leaves its source classified
 * read-only, and the source's storage was already dead by then.
 *
 * **Places rooted in a raw pointer are not checked against each other.** placesOverlap() answers no
 * for two of them, so `*p` and `*q` never conflict however they were derived. That is what `%T`
 * means and what makes Native's `borrow` the deliberate seam it is - a collection written over raw
 * storage is trusted about aliasing inside itself, and owes its callers a `return` marker that
 * makes the outside checkable.
 *
 * **Regions.** The storage decision is between the frame and the heap; StorageClass::Region is
 * reserved and never selected. Implementation-Regions.md part 4 is the third case in this
 * decision rather than a new pass, which is why it was left out rather than approximated.
 *
 * **Repr variants beyond "in memory or not".** There is no packing and no niche yet, so the only
 * two representations that differ are storage and no storage - which is what resolve/lower.cpp's
 * scalarization spends the demand result on. A read-only variant that differs in *layout* needs
 * Implementation-Repr.md's work first, and a materialize/thaw conversion at the boundaries where
 * an unspecialized ABI requires the canonical one.
 *
 * **A woken function is re-analyzed in full.** settleSummaries wakes a caller only when a callee's
 * summary actually moved, so the whole-program repetition is gone; what a wakeup still costs is
 * every intraprocedural pass over that one body, including liveness and the ownership lattice, which
 * cannot have changed - only the four summary-reading rules can. Splitting the per-function work
 * into the part that depends on callees and the part that does not is what would narrow it, and it
 * is worth doing only once something measures the wakeups as a cost.
 *
 * **The checked reference rungs.** `Ref` and `RegionPtr` classify conservatively in ownershipOf()
 * and are not constructible yet, so nothing exercises them.
 *
 * **An InstCallDyn's arguments are assumed retained**, since there is no callee to have a summary.
 * That is the same answer an opaque direct call gets, and it has the same consequence: a root handed
 * to a function value goes to the heap. What it no longer costs is a leak - the retention is
 * classified as a reference kept rather than as ownership handed over, so the frame still releases
 * the storage - and what it no longer discards is the signature: the declared `return` group is read
 * for the result's provenance and for the extent of the loans the arguments create, because those
 * are contracts the function *type* states and FunArg carries them for exactly this position.
 *
 * The remaining half is retention itself, which a function type cannot state: `(Int) -> Int` says
 * nothing about whether the callee keeps what it was given, so every argument is assumed kept. A
 * marker on FunArg saying otherwise is what would narrow it, and it would have to be checked in
 * every lambda and thunk that becomes a value of that type.
 *
 * **A retained root is still heap-placed.** Since the frame both allocates and releases it, the
 * heap buys nothing over the frame here: what the retention says is that a *reference* may outlive
 * the call, and neither storage class makes that reference valid afterwards. Leaving `escaped`
 * driving the storage class is the conservative reading of a fact the pass did not prove, and
 * narrowing it means deciding what a reference kept past a call is allowed to mean at all.
 * ---------------------------------------------------------------------------------------------
 */
