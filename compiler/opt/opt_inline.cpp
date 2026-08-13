#include "opt_pass.h"

/*
 * Inlining: a call replaced by a copy of what it would have done.
 *
 * There are two shapes of that copy here and the difference between them is the whole of what makes
 * the second one harder than the first.
 *
 *  - A **straight-line** callee - one block ending in a `ret` - is spliced into the caller's block
 *    where the call was. There is no control flow to graft, no block to split, no phi to build for a
 *    result several `ret`s agree on, and no successor's phi inputs to rename. The whole of it is a
 *    value map and a local map;
 *  - a callee with **control flow** needs all four. The caller's block is cut in two at the call, the
 *    callee's blocks are cloned into the gap, each of its `ret`s becomes a jump to the second half,
 *    and where more than one of them returns a value the second half opens with a phi over them.
 *
 * The first is where most of the wins are and it stays the fast path, because what it leaves behind
 * is one block rather than five: the callee's `alloc` lands in the caller's own block, which is where
 * the block-local passes after this one can still see it. The second exists because a great many
 * bodies worth inlining are not straight-line - anything with an `if` in it - and because a `@lazy`
 * argument is a body the caller wrote, spliced into a callee that asked for it, with whatever control
 * flow the caller happened to put there.
 *
 * ## What it is for, which is not "removing the call"
 *
 * Removing a call is the small part. What inlining buys at this altitude is that the *other* passes
 * can then see through it:
 *
 *  - a constructor inlined into its caller becomes an `alloc` and some `init`s in the caller's own
 *    block, which is exactly the shape opt_scalar.cpp takes apart - so the record is never built at
 *    all. On a managed host that is an allocation and a hidden class that stop existing;
 *  - a mutator taking `&x` becomes reads and writes of the caller's own place, which forwarding then
 *    answers - so the borrow, and often the local behind it, go away with it;
 *  - a callee called with constants folds against them, and the dead-value pass collects whatever
 *    that made unreachable. This is the one that pays on every target, because it is the only way a
 *    constant crosses a call boundary in this compiler at all. With control flow in the picture it
 *    is also how a branch folds: a predicate the caller passed as a literal turns the callee's `je`
 *    into a `jmp`, and opt_branch.cpp then deletes the arm that stopped being reachable.
 *
 * Which is why this runs program-wide before any function is optimized, in the same place and for
 * the same reason `flattenArguments` does: what it leaves behind is work for the passes after it.
 *
 * ## The heuristics, and why they differ per target
 *
 * `InlinePolicy` is the whole cost model and it is a table rather than a computation. Two axes:
 *
 *  - **the level**, which is `-Os` against `-Ofast` and nothing more subtle. `Size` inlines only
 *    where the program cannot grow - a callee with exactly one call site in the program, which takes
 *    a body away rather than copying one - and `Speed` raises every budget.
 *  - **the target family**, which changes the *shape* of the answer rather than its size. See
 *    `policyFor` for the three rules that differ and the reasoning behind each.
 *
 * Every bonus is a budget increase rather than a decision, so they compose: a callee taking a
 * mutable borrow *and* called with a constant *and* called once gets all three, and one that is
 * simply small needs none of them.
 *
 * One of them is priced against something the callee's own body does not show, and it is the reason
 * a *table* is enough here rather than a cost model. `accessor` weighs a body that reads through what
 * it was handed and does nothing else - `length(self)`, `stringUnit(self, i)`, `xs[i]` - and what
 * such a call costs is not the call: it is that the address the callee computed cannot leave it. A
 * `call` is a barrier to loop hoisting, to place forwarding and to check discharge all three, so an
 * accessor left as a call is an invariant address recomputed per iteration, a load nothing may
 * forward, and a bounds check nothing may prove twice. `size` counts a container accessor at eight or
 * ten instructions, which is over the base budget by construction. See `isAccessorBody`.
 *
 * ## What is declined, and why each one
 *
 * The ownership instructions are the list that matters. `Drop`, `Move`, `Swap` and `Exchange` are
 * transfers the analyses already decided and spent, and copying one into a caller is asserting that
 * the decision travels - which for a drop is a double free if it does not. So a body containing any
 * of them is declined outright, as is a callee any of whose types owes a teardown. That is stricter
 * than necessary and it is the right place to start: what is left is bodies whose whole content is
 * computation, reads, writes and calls, and every case in the paragraphs above is one of those.
 *
 * A `->` sink parameter is declined for the same reason from the other side, and a `return`
 * parameter because the loan the caller took was sized against the summary rather than the body.
 *
 * A parameter of a memory type passed by value is *not* declined, and used to be. It arrives as the
 * caller's storage rather than as a copy of it, so the callee's places are rooted in a local this
 * frame does not have - but the caller's argument is a place, which is exactly the root they should
 * be rebuilt against. `Binding::Memory` is that rewrite and it is the same one the `&` case already
 * had. Until it existed, every reader of a borrowed container was a call whatever its size:
 * `releaseRun(self: Run(a))` is one comparison and `length(self: Flat(a))` is one load, and both of
 * them were permanent calls - which is what Implementation-Containers.md §13.2 is about.
 *
 * ## The dynamic form, and why there is no direct-call form beside it
 *
 * A `calldyn` through a function value this frame built is inlined too - `inlineDynamicCall`, which
 * reads the code word and the environment word out of the storage and binds the environment as the
 * callee's first parameter. It is what flattens an adaptor chain, because the chain is a loop in one
 * function calling a continuation in another *through a value*: nothing sees into that call until it
 * is resolved, and once the body is spliced the captures it reads are the caller's own locals, which
 * is what the next round then resolves in turn.
 *
 * What it deliberately does not do is produce a direct `Call` to the lifted lambda. Such a call does
 * not exist in this IR (see opt_closure.cpp) and the convention it would need - the environment in
 * front of the declared arguments - is one only the *value* form spells, and one codegen/js does not
 * spell at all. So the splice leaves a body, never a call to one.
 *
 * A **recursive** callee is declined, and that is a rule the straight-line half never had to state:
 * a body that calls itself almost always branches on something first, so refusing branches refused
 * recursion as a side effect. It does not any more. Copying a recursive body into a caller copies the
 * recursive call with it, and the copy is *unrolling* rather than inlining - a different
 * transformation, with a cost model this table does not have and a growth rate the round budget
 * bounds only by accident. So the call graph's cycles are found once and a function in one is
 * refused. What remains bounded by the round budget is only the honest case: a chain of callees each
 * of which calls the next.
 *
 * The one exception is `collapsesCycle`, and it is the case where the argument above does not hold:
 * a *different* member of the caller's own cycle, taken once. That is a collapse rather than an
 * unrolling - the copy's calls come back to the caller, so a mutual pair becomes one self-recursive
 * function - and the compiler writes such pairs itself, one per type in a recursive type's derived
 * teardown.
 */

namespace {

/*
 * What a call site is worth, in instructions.
 *
 * `budget` is the size a callee may be for the call to be worth inlining; everything else adjusts
 * it up or down for what this particular call site looks like. Numbers rather than named cases so
 * that a call site with two reasons to be inlined gets both.
 */
struct InlinePolicy {
    U32 budget = 0;

    // A callee whose only call site in the whole program is this one. Inlining it removes a body
    // rather than copying one, so this is the one bonus that is a size win as well as a speed one.
    U32 soleCallSite = 0;

    // Per argument that is a constant, capped at `constantCap` arguments so that a wide signature
    // of literals does not buy an unbounded body.
    U32 constantArgument = 0;
    U32 constantCap = 4;

    // A parameter taken by mutable borrow, and a result the target holds in memory. Both are
    // allocations the caller made for the call and both stop existing when it goes away.
    U32 mutableBorrow = 0;
    U32 memoryResult = 0;

    /*
     * A result that is a *reference* to something the target does not hold as an object.
     *
     * The same allocation `memoryResult` prices, reached from the other side. On a managed target a
     * reference to a non-object is an object of its own - `{$o, $k, $s}` naming the slot the value
     * lives in - and returning one is the use that forces it to be built, since JS has no
     * multi-value return. Splicing the call away leaves the callee's borrow in the caller's own
     * body, where its every use is a place root and nothing is allocated at all.
     *
     * Zero natively, where a returned borrow is an address in a register and there was never
     * anything to remove - which is the same shape `mutableBorrow` and `memoryResult` have and for
     * the same reason.
     */
    U32 borrowResult = 0;

    /*
     * A body that is a guarded read of what the call handed it and nothing else - see
     * `isAccessorBody`, which is what decides the shape.
     *
     * The one term here that is priced against what the *caller* then does rather than against what
     * the callee contains, and it is the only way an accessor is ever worth its size. `size` counts
     * a container accessor's projections, its casts and the compare its bounds check is - which is
     * eight or ten instructions for `xs[i]` - so an accessor is over the base budget by construction
     * while being exactly the call that must not survive. What the copy buys is not the call: it is
     * that the address the callee computed lands in the caller's own block, where the loop passes
     * can hoist the invariant half of it, `opt_place.cpp` can forward the load, and
     * `opt_discharge.cpp` can see the check is one it has already made.
     *
     * None of which is visible in the callee, and none of it survives a call - a `call` is a
     * barrier to every one of those three. So the bonus is deliberately narrow rather than a raised
     * `budget`: measured over `test/bench/programs`, `hashOf`'s inner loop is a permanent call per
     * element without it and eight call-free instructions with it, exactly two callees in the ten
     * programs qualify at all, and nine of the ten do not move by a byte. Raising the budget to reach
     * the same two takes every ten-instruction body in the prelude with them - see §9.3 of
     * `test/bench/findings.md`, which also has why the attribute is not the answer here.
     *
     * Target-independent, on the same grounds as `constantArgument` and `soleCallSite`: what it
     * prices is that a computation crosses the call boundary at all, and a boundary is a boundary on
     * both targets. On JS these bodies are a `HostField` or a `HostCall` and are small enough to
     * qualify without it, so the term is what makes the two targets agree here rather than what
     * makes them differ.
     */
    U32 accessor = 0;

    /*
     * Per argument that is a function value this frame built and that the callee *reaches*.
     *
     * The second term the table prices against what the copy makes possible rather than against
     * what the callee contains, and it is the one that flattens an adaptor chain. A closure is
     * `{code, env}` in the frame that built it and an address in every frame below; a callee holding
     * a `calldyn` on one it was handed is an indirect call per iteration that no pass can see into,
     * and the *same* body copied into the frame that built the closure is a call this pass then
     * resolves and splices - see `inlineDynamicCall`. So what the bonus buys is not the call it
     * removes but the two below it.
     *
     * Large, because what it has to reach is a whole loop rather than a small body: `each` in
     * `test/bench/programs/Pipeline.yana` is thirty instructions over eight blocks, and it is the
     * function every `for x in mapped(xs, f)` in the program is. Narrow enough to afford it - both
     * halves have to hold, so a callee that never mentions its function parameter gets nothing, and
     * a site passing a closure whose code word merges gets nothing either.
     *
     * **And it is the one term that raises the ceiling with it** - see `worthInlining`, which is
     * where that argument is, and `Parameter::used` for why "reaches" rather than "calls".
     */
    U32 closureArgument = 0;

    /*
     * Per indirect call the callee holds, where this site is itself one this pass resolved - capped
     * at `chainedCap`, so that a body full of them does not buy an unbounded copy.
     *
     * `closureArgument` read from the other end, and the same argument. That term prices a closure
     * arriving as an *argument*; this one prices the link below it, where the closure arrives in the
     * environment of a call that has just been resolved. `Collections.continuation$1` is the shape:
     * the chunk walker reads the loop body out of `[%env].body` and calls it, and neither the code
     * word nor the environment is anything the walker's own frame can name. Copied into the frame
     * that built them, both are - the environment is re-rooted at the caller's own storage, the
     * forwarding turns the read of the body word into the function value the caller wrote there, and
     * `knownCallee` resolves the call that was opaque.
     *
     * So what this buys is the *chain*: the second link of `for v in vectors(xs)` costs a body of
     * sixty against a ceiling of forty-eight, and what stands behind it is the third link, the loop
     * body itself, and - once the accumulator is a local rather than a capture - the promotion in
     * `lower_promote.cpp` that §34.4 measured at 2.5x on the loop it applies to.
     *
     * Narrow by construction: both halves have to hold, and the site half is only ever true of a
     * `calldyn` this pass resolved out of a function value the frame built, which is an adaptor
     * chain and nothing else. A body that merely *contains* an indirect call, reached by an ordinary
     * call, earns nothing here.
     */
    U32 chainedCall = 0;
    U32 chainedCap = 2;

    /*
     * Where the copy removes the *last* call from the caller: the callee performs none, and this
     * site is the only one the caller has.
     *
     * The third term priced against what the copy makes possible rather than against what the callee
     * contains, and the only one that is about neither body - it is about the caller's frame. A
     * function containing a call has to hold every value live across it somewhere the call cannot
     * clobber, which on this target means a callee-saved register pushed in the prologue and popped
     * in the epilogue, plus the moves that get the arguments into place. A function containing none
     * has no prologue at all: `insertMasked` in `test/bench/programs/Hash.yana` spends five pushes,
     * five moves and five pops on a call to eight instructions of arithmetic, and loses all fifteen
     * when the eight are copied in. That is why the program gets *smaller* here rather than larger,
     * which is not something any other term on this table can say.
     *
     * Both halves are checked and neither is a heuristic. A callee that calls anything brings the
     * frame back with it, and a caller with a second call keeps its prologue for that one - so what
     * this admits is exactly the case where the last call goes away, and nothing near it.
     *
     * **Native only**, on the same grounds that split `mutableBorrow` and `borrowResult` the other
     * way: what it prices is a register allocator's frame, and a managed host has neither. On JS a
     * call is a call whatever else the function contains, so the term would be paying for nothing.
     */
    U32 leafCaller = 0;

    /*
     * Where the copy collapses a cycle in the call graph - see `collapsesCycle`.
     *
     * The fourth term priced against what the copy makes possible, and the one furthest from what
     * the table otherwise measures: every other row prices a call that goes away *once*, and this
     * one prices a call that goes away once per level of a recursion whose depth nothing here knows.
     * `reclaim$Tree` and `reclaim$Maybe(Tree)` in `test/bench/programs/Tree.yana` are two frames per
     * node of a sixteen-thousand-node structure; the copy makes them one, and what the ordinary
     * arithmetic sees is a five-instruction body against a limit of three - the base budget, less the
     * repeat penalty for two sites, less three blocks of graft. Refused, for a body whose call is
     * executed 16383 times.
     *
     * So the term is large and its gate is narrow: only `collapsesCycle` admits a recursive callee at
     * all, only one member of the caller's own cycle is ever taken, and the ceiling still applies -
     * which is what keeps a large mutual pair out. What it costs where it fires is one copy of a
     * small body; the cycle is what is being removed, not the call.
     *
     * Zero at `InlineLevel::Size`, where the trade is the wrong way round: the copy is growth in
     * every body that takes one, and the frames it saves are time rather than bytes.
     */
    U32 cycleCollapse = 0;

    // Subtracted where the callee is called from more than one place, and again where it is called
    // from many. What this prices is code growth, which is paid once per call site.
    U32 repeatedPenalty = 0;
    U32 manyCallSites = 0;
    U32 manyPenalty = 0;

    /*
     * Subtracted per block past the first, and the ceiling on how many of them there may be.
     *
     * What this prices is not the instructions - those are already in `size` - but what the *graft*
     * costs the passes downstream. A callee spliced as one block leaves its `alloc` where the
     * caller's own block-local passes can see it; one spliced as five blocks leaves the same `alloc`
     * behind a branch, where forwarding and scalarization stop answering. So a branching callee has
     * to be worth more than a straight-line one of the same size, and `maxBlocks` is the point past
     * which no bonus makes it worth anything - a hard cap for the same reason `ceiling` is one.
     */
    U32 blockCost = 0;
    U32 maxBlocks = 0;

    /*
     * `@inline` on the declaration - see readInlineAttribute in resolve/module.cpp.
     *
     * A budget term like every other, which is the whole of what makes it an honest hint: it says
     * "weigh this callee as if it were smaller", not "inline this". A callee the checks in
     * `describe` refuse is still refused however large a number goes here, and the ceiling below
     * still applies - so the attribute cannot ask for something the pass would then quietly not do.
     *
     * Large enough to carry a callee well past the base budget, because a caller that wrote it knows
     * something about the payoff that the body does not show - a loop this sits in, or folding that
     * only happens two passes later. Not infinite, for the reason the ceiling exists.
     */
    U32 requested = 0;

    // The size past which no bonus helps. A ceiling rather than another term, because the thing it
    // exists to prevent is a large body being copied on the strength of enough small reasons.
    U32 ceiling = 0;
};

/*
 * The table, by level and by target family.
 *
 * Three rules differ between the families and all three come from the same fact - that a managed
 * host does not have an optimizing backend under it, and does have a collector:
 *
 *  - **a managed target pays much more for an allocation**, so `mutableBorrow` and `memoryResult`
 *    are large there and small natively. On JS a record that stays a record is an object with a
 *    hidden class; removing the call is what lets opt_scalar.cpp remove the object. Natively the
 *    same record is bytes in a frame the function already has, and LLVM would have inlined the
 *    callee anyway - so the bonus buys little and is priced accordingly;
 *  - **a managed target pays much more for code size**, so `repeatedPenalty` and `manyPenalty` are
 *    larger and `ceiling` lower. Emitted JS is source text the host parses, and V8 has an inlining
 *    budget of its own that a function grown past it stops qualifying for - so inlining a large
 *    callee into six call sites can lose twice, once in bytes and once by pushing each caller out of
 *    the host's own budget. That is the user-visible half of "avoid inlining large functions that
 *    are called multiple times";
 *  - **a managed target pays much more for a block**, so `blockCost` is larger there. Natively a
 *    block is a label and a jump. On JS it is not even that: codegen/js/flow.cpp has to *recover* an
 *    `if` or a `for(;;)` from the graph, and every block spliced into a caller is one more join for
 *    that recovery to find a structured form of.
 *
 * What does *not* differ is the constant-argument bonus and the sole-call-site bonus, because
 * neither is about the machine. A callee folded against its arguments is smaller after inlining
 * than the call was, and a callee with one call site leaves nothing behind on either target.
 */
InlinePolicy policyFor(InlineLevel level, TargetFamily family) {
    auto managed = family == TargetFamily::Managed;
    InlinePolicy policy;

    switch(level) {
        case InlineLevel::None:
            return policy;

        case InlineLevel::Size:
            // Nothing but the case that cannot grow the program, which is why the base budget is
            // zero: a callee qualifies here only through `soleCallSite`. `blockCost` is zero for the
            // same reason - a body that moves rather than being copied costs no blocks either.
            policy.budget = 0;
            policy.soleCallSite = 10;
            policy.constantArgument = 0;
            policy.mutableBorrow = 0;
            policy.memoryResult = 0;
            policy.borrowResult = 0;

            // Zero here for the reason the base budget is: an accessor with thirteen call sites is
            // thirteen copies of it, and the whole of what this level will pay for is a body that
            // moves. The argument for the bonus is a speed one and this is the level that declines
            // to hear one.
            policy.accessor = 0;
            policy.closureArgument = 0;

            // And zero here on the same terms: what it prices is a chain of bodies copied into one
            // frame, which is growth in every one of them.
            policy.chainedCall = 0;

            // Zero here for the reason every speed argument is at this level, and this one has a
            // size argument as well - see the term. It is declined anyway: what the level pays for
            // is a body that moves, and a body with one call site moves already.
            policy.leafCaller = 0;

            // And zero here on the same terms: a collapse is a copy of a body, and this level pays
            // for a body that moves rather than for one that runs fewer times.
            policy.cycleCollapse = 0;

            policy.repeatedPenalty = 0;
            policy.manyCallSites = 2;
            policy.manyPenalty = 0;
            policy.blockCost = 0;
            policy.maxBlocks = 8;
            policy.requested = 24;
            policy.ceiling = 24;
            break;

        case InlineLevel::Balanced:
            policy.budget = 8;
            policy.soleCallSite = 12;
            policy.constantArgument = 3;
            policy.mutableBorrow = managed ? 10 : 3;
            policy.memoryResult = managed ? 12 : 4;
            policy.borrowResult = managed ? 12 : 0;
            policy.accessor = 6;
            policy.closureArgument = 28;
            policy.chainedCall = 28;
            policy.leafCaller = managed ? 0 : 8;
            policy.cycleCollapse = 16;

            // Two and four rather than one and two on the native side, and what moved them is
            // `settleCallee` rather than anything about growth. A size measured before the callee
            // was folded was two to three times the truth, so a penalty of one was being subtracted
            // from a limit weighed against an inflated number and did most of its work by accident;
            // with the sizes honest the old pair let repeated bodies through that nobody wanted
            // copied. Measured, the two of them are 938 fewer bytes over the 152 `test/resolve`
            // executables and 259 over the ten programs, for nothing on the clock.
            policy.repeatedPenalty = managed ? 3 : 2;
            policy.manyCallSites = managed ? 4 : 8;
            policy.manyPenalty = managed ? 6 : 4;
            policy.blockCost = managed ? 3 : 1;

            // Sixteen rather than eight, which is `Speed`'s own value. Eight was under the shape
            // every container hands back: `slice` is four clamps and a descriptor, thirteen blocks,
            // and the cap refused it before any bonus was weighed. Measured over the corpus, the
            // pair of this and the memory-result term in `worthInlining` is 29 fewer lines of
            // emitted JavaScript and 0.15% more lowered native.
            policy.maxBlocks = 16;
            policy.requested = 32;
            policy.ceiling = managed ? 40 : 48;
            break;

        case InlineLevel::Speed:
            policy.budget = 20;
            policy.soleCallSite = 32;
            policy.constantArgument = 5;
            policy.mutableBorrow = managed ? 16 : 6;
            policy.memoryResult = managed ? 20 : 8;
            policy.borrowResult = managed ? 20 : 0;
            policy.accessor = 12;
            policy.closureArgument = 36;
            policy.chainedCall = 36;
            policy.leafCaller = managed ? 0 : 12;
            policy.cycleCollapse = 24;
            policy.repeatedPenalty = managed ? 2 : 0;
            policy.manyCallSites = managed ? 8 : 16;
            policy.manyPenalty = managed ? 4 : 1;
            policy.blockCost = managed ? 2 : 1;
            policy.maxBlocks = 16;
            policy.requested = 64;
            policy.ceiling = managed ? 80 : 120;
            break;
    }

    return policy;
}

// How one parameter arrives, which decides what the callee's uses of it become.
enum class Binding: U8 {
    // A register value. The callee reads the `Arg` itself, and every read becomes the caller's
    // argument value.
    Value,

    // A `&` parameter: the callee has a local whose storage is the caller's, and reads and writes
    // go through places rooted in it. Those places become places rooted in the caller's borrow.
    Borrowed,

    /*
     * A parameter of a memory type passed by the default convention, which is the caller's storage
     * under a second name rather than a copy of it: the argument at the call site is a value the
     * caller loaded out of a place, and what the callee is handed is that place's address.
     *
     * So the rewrite is the same one `Borrowed` gets and it re-roots at a *place* rather than at a
     * borrow - the caller's own path, with the callee's path hung off the end of it. Which place
     * that is depends on the call site rather than on the declaration, so `storage` below is filled
     * in per call rather than by `describe`.
     */
    Memory,

    /*
     * A raw pointer parameter whose place the caller knows, which is what a lifted lambda's
     * environment is - see `inlineDynamicCall`.
     *
     * The callee reads its captures through `[%env].f`, a place at a *pointer* root, and the site
     * holds `addressof %local` - so the caller knows both the storage and its type. Re-rooting at
     * that place is what makes the copy type-check: the environment word is `%()` in a function
     * value, and a `%()` root with a field step on it is a place the verifier is right to refuse.
     *
     * Kept apart from `Memory` because the root it rewrites is a different one. A memory parameter's
     * places are rooted in the callee's *local*, and this one's are rooted in the parameter value
     * itself, so the two match on different things and neither could stand in for the other.
     */
    Pointer,
};

struct Parameter {
    Binding binding = Binding::Value;

    /*
     * Whether the body reaches this parameter at all, which is half of what makes a closure passed
     * here worth a budget of its own - the other half is that the argument is one this frame built,
     * and `closureBonus` asks that separately. See `InlinePolicy::closureArgument`.
     *
     * **Reaching it rather than calling it**, which is what the iteration protocol needs: `vectors`
     * yields from inside `for chunk in chunks(self)`, so it never calls the continuation it was
     * handed - it captures it into the environment of the closure it builds for the chunk walker,
     * and the `calldyn` is a level further down in a body this one only names. Read as "calls it",
     * every `for v in vectors(xs)` in the language scored a bonus of zero, and the term reached only
     * the adaptors that call their function directly.
     *
     * A parameter the body never mentions earns nothing, which is the whole of what this excludes:
     * a closure nothing reads is one the copy cannot resolve a call through either.
     */
    bool used = false;

    // The callee local the two re-rooted cases rewrite away, and `kNone` for the value case.
    U32 local = maxLimit<U32>;

    // The callee's `Arg` for this position, which `Binding::Pointer` matches a place's root against.
    ModulePtr<Value> arg = nullptr;

    // Where the caller keeps what it passed, for `Memory`. Empty in the other two cases.
    Place storage;
};

struct Candidate {
    ModulePtr<Function> pointer = nullptr;
    Function* callee = nullptr;
    SmallArray<Parameter, 8> parameters;

    /*
     * What the site passes, positionally, filled in once the site is known.
     *
     * Read rather than the `InstCall`'s own list because not every site is a call: a teardown is
     * reached from an `InstDrop`, which passes nothing at all and hands over a *place* instead. That
     * shape has no argument list to index, so the clone below is written against this one.
     */
    SmallArray<ModulePtr<Value>, 8> arguments;

    /*
     * The callee's reachable blocks in reverse postorder, and the ones ending in `ret`.
     *
     * The order is what the clone below rests on: a non-phi operand is dominated by its definition,
     * so a walk in this order reaches every definition before the use that names it and the value
     * map is complete by the time it is asked. A phi is the exception SSA makes to that, and is why
     * phis are created ahead of everything else rather than in place.
     *
     * Unreachable blocks are left out rather than cloned into unreachable copies of themselves. The
     * one thing that costs is a phi alternative arriving from one, which is dropped with it - and
     * dropping it is correct rather than approximate, since there is no edge for it to arrive over.
     */
    SmallArray<ModulePtr<Block>, 12> blocks;
    SmallArray<ModulePtr<Block>, 4> returns;

    // The callee local holding what a memory-typed result is returned out of, and `kNone` where the
    // result is a register value or nothing at all.
    U32 resultLocal = maxLimit<U32>;

    /*
     * A memory-typed result several `ret`s produce, which is one slot in the caller written on
     * whichever path runs rather than one callee local renamed - see `describe`.
     *
     * `callerResultSlot` is that slot, filled in by `graft` once the site is known, and it is the
     * value the call becomes.
     */
    bool copiedResult = false;
    U32 callerResultSlot = maxLimit<U32>;

    U32 size = 0;

    // What `isAccessorBody` decided, which `policy.accessor` prices. A property of the declaration
    // like everything else here, so it is computed with the rest of the description rather than per
    // call site.
    bool accessor = false;

    // Whether the body performs no call of its own, which is half of what `policy.leafCaller`
    // prices. The other half is the site's, and is asked there.
    bool callFree = true;

    /*
     * How many indirect calls the body holds, and whether the site being weighed is one this pass
     * resolved out of a function value - the two halves of `policy.chainedCall`.
     *
     * `dynamicCalls` is the callee's own and is counted with the rest of the description; `dynamic`
     * belongs to the site and is set by `inlineDynamicCall`.
     */
    U32 dynamicCalls = 0;
    bool dynamic = false;

    bool isStraightLine() const { return blocks.size() == 1; }

    static constexpr U32 kNone = maxLimit<U32>;
};

/*
 * Which functions can reach themselves through a chain of calls, found once for the whole program.
 *
 * Tarjan's algorithm rather than a reachability closure, because the question is exactly "is this
 * function in a cycle" and a strongly connected component is exactly a cycle: a component with more
 * than one member is a mutual recursion, and a self-edge is the one-member case the component
 * numbering cannot tell from an ordinary function on its own.
 *
 * Computed before the rounds and not recomputed between them, which is sound rather than a shortcut:
 * inlining copies a callee's calls into its caller, so a caller gains edges - but only edges to
 * things it could already reach through the call that went away. No cycle is created and none is
 * destroyed, so the answer is the same on every round.
 *
 * `GenCall` is an edge here and is not a call this pass will ever clone. That is deliberate: what
 * the graph is for is deciding whether a *body* is recursive, and a body that reaches itself only
 * through a class dispatch is as recursive as one that does not.
 */
void findRecursion(OptContext& opt, HashMap<U32, bool>& recursive, HashMap<U32, U32>& cycle) {
    Array<ModulePtr<Function>> nodes;
    HashMap<U32, U32> index;

    // Sized before the walk rather than grown into. Both tables end up holding one entry per
    // function in the program, and a hash map reached by doubling from empty rehashes its way there
    // - which for a program with a prelude is a dozen allocations and a dozen full rehashes.
    Size functionCount = 0;
    for(auto module: opt.program.modules) functionCount += module->functionOrder.size();

    index.reserve(functionCount);
    recursive.reserve(functionCount);
    cycle.reserve(functionCount);
    nodes.reserve(U32(functionCount));

    for(auto module: opt.program.modules) {
        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto entry = index.add(U32(pointer));
            if(entry.existed) continue;

            *entry.value = U32(nodes.size());
            nodes.push(pointer);
        }
    }

    /*
     * The call graph, as one array of targets with a start offset per node rather than an array per
     * node. The second walk visits the nodes in order, so a node's edges are the run between its
     * own start and the next one's - which is the whole of what the array-per-node bought, at two
     * allocations for the graph instead of one per function in the program.
     */
    Array<U32> edgeData;
    Array<U32> edgeStart;

    // Second walk rather than one, because an edge can name a function the first walk has not
    // numbered yet - which is every forward reference and every call into another module.
    for(Size i = 0; i < nodes.size(); i++) {
        edgeStart.push(U32(edgeData.size()));

        for(auto blockPointer: opt.local[nodes[i]]->blocks.contents(opt.local)) {
            for(auto instructionPointer: opt.local[blockPointer]->instructions(opt.local)) {
                auto& instruction = *opt.local[instructionPointer];

                ModulePtr<Function> callee = nullptr;
                if(instruction.kind == Value::Call) callee = ((InstCall&)instruction).callee;
                else if(instruction.kind == Value::GenCall) callee = ((InstGenCall&)instruction).callee;

                if(!callee) continue;

                auto found = index.getValue(U32(callee));
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

    /*
     * Explicit stacks rather than recursion: a call chain in a large program is deeper than this
     * process's own stack is willing to be, and the walk needs no more of a frame than the node and
     * how far through its edges it got.
     *
     * `component` is the one that decides the answer, and membership is read off it as each
     * component is popped rather than off the finished numbering. The numbering does not answer it:
     * a member reached through a second back edge keeps *that* edge's number rather than its
     * component head's, so two members of one cycle can end up carrying two different lowlinks.
     */
    Array<U32> component;
    Array<U32> pending;
    Array<Size> next;

    // Emptied per component rather than built per component, which for a program of five hundred
    // functions is five hundred one-member components and five hundred allocations.
    Array<U32> members;
    U32 counter = 1;

    // Which cycle a function is in, numbered from one so that "not in one" and "component zero" are
    // different answers. Only a component that is a cycle is numbered - the ordinary one-member ones
    // are the whole program and naming them would be a map the size of it.
    U32 cycleCounter = 1;

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
                // Everything pushed since this node was first reached is the component it heads.
                members.clear();
                while(component.size()) {
                    auto member = component.pop().unwrap();
                    onStack->set(member, false);
                    members.push(member);

                    if(member == node) break;
                }

                // A one-member component is a cycle only through an edge to itself, which is the
                // case the component numbering cannot report - a cycle of length one.
                if(members.size() == 1 && !callsItself(node)) continue;

                auto id = cycleCounter++;
                for(auto member: members) {
                    recursive.add(U32(nodes[member]), true);
                    cycle.add(U32(nodes[member]), id);
                }
            }

            if(pending.size() && lowlink[node] < lowlink[pending[pending.size() - 1]]) {
                lowlink[pending[pending.size() - 1]] = lowlink[node];
            }
        }
    }
}

/*
 * A cap on the folding below, on the same terms as every other round limit here: each pass strictly
 * removes something, so the loop ends on its own and this is what turns a future pass that oscillates
 * into a slow compile rather than a hang.
 */
constexpr Size kMaxSettleRounds = 4;

struct Inliner {
    OptContext& opt;
    InlinePolicy policy;
    HashMap<U32, bool> taken;
    HashMap<U32, bool> recursive;
    HashMap<U32, U32> callSites;

    /*
     * How many `symbol` instructions in the program name each function, counted beside the call
     * sites and by the same walk.
     *
     * The other way into a body, and for a lifted lambda it is the only one: nothing calls
     * `Collections.continuation$1` by name, so `callSites` says zero about a body every chunk of
     * every container goes through. One code word is one function value built in one place, which is
     * what `movesIntoSite` reads it for.
     */
    HashMap<U32, U32> codeWords;

    /*
     * The three tables `collapsesCycle` is about, and they bound it between them.
     *
     * `cycle` is which cycle of the call graph a recursive function is in, so that "a member of the
     * caller's own" is a question rather than a guess. `absorbed` is which member each caller has
     * taken - the callee rather than a flag, because every *site* of the same one is the rest of the
     * same collapse while a second callee is a second cycle. `collapsed` is every call site this pass
     * has written, which is what makes the whole thing terminate: a copy of a cycle member brings the
     * cycle's calls in with it, and taking one of *those* is the second level of an unrolling that
     * has no end - `B` calling itself, or `B` copied into `A` and the enlarged `A` then copied back.
     */
    HashMap<U32, U32> cycle;
    HashMap<U32, U32> absorbed;
    HashMap<U32, bool> collapsed;

    // Which site is being considered, for the one rule that is about the site rather than about
    // either body. Set by each of the three site functions in front of `describe`.
    ModulePtr<Inst> site = nullptr;

    // Which callees have been folded to a fixed point once - see `settleCallee`. Kept across the
    // rounds rather than per round: a body nothing was spliced into is unchanged between them, and
    // one something was spliced into is settled by `runFunction` where that happened.
    HashMap<U32, bool> settled;

    /*
     * Whether cloning this instruction into another function is something this pass knows how to do.
     *
     * An allow-list rather than a deny-list, and the header comment says why the ownership four are
     * out. The rest is what is left: computation, storage, reads, writes, borrows and calls.
     *
     * `Native` and `CallDyn` were declined not because they are unsound but because each carries
     * state - an intrinsic's operation and arguments, a signature - that had to be copied correctly
     * and was not exercised by anything this inlined. It is now: a host node's arguments are *uses*
     * and it owns nothing (see resolve/host.cpp), so copying `op`, `method` and the argument list is
     * the whole of it - and `length(xs)`, one `.length` read behind a permanent call, is what that
     * makes reachable.
     *
     * `GenCall` stays out, and for a reason that is not "not exercised yet": `fill.forwarded`,
     * `classSlot` and `classPath` are slot numbers in the *enclosing function's* generic
     * environment, and the enclosing function is exactly what a graft changes. A copy of one into
     * another body would read some other schema's slots, which is a miscompile rather than a
     * missing feature. It only ever appears in a generic body, so what would have to be checked
     * first is that both schemas agree.
     *
     * ## The ownership four are all in
     *
     * The header's rule is that copying an ownership instruction asserts the decision travels. A
     * graft is what *makes* it travel: the whole body is copied, drops and all, and the copy runs
     * once per call exactly as the callee did. What the rule is really about is a decision copied
     * away from the rest of the decision it belongs to, and that is not this.
     *
     * `Move` was admitted first, on the additional ground that it is not a decision at all by the
     * time this runs - it is the relocation itself, with no lower form to discharge it into. The
     * other three do still expand into something, and that used to be the reason they were out:
     * `dischargeOwnership` ran in front of this pass and removed them, so admitting them here would
     * have been admitting a shape that never arrived.
     *
     * That ordering is now the other way round - the discharge runs *after* this pass, so that the
     * escape analysis can be re-run over the collapsed call graph while a drop is still the
     * instruction it ignores (see reselectStorage, and opt.cpp). Which makes these three the
     * ordinary case rather than an absent one, and the paragraph above is the whole of what makes
     * copying them sound: the body is copied entire, so each runs once per call exactly as it did.
     *
     * The hazard is re-rooting rather than the instruction: a cloned `Move` or `Drop` whose place
     * was rewritten to name the *caller's* storage would empty or release a slot the caller's
     * ownership state knows nothing about. Two guards already exclude it - the borrow check rejects
     * a move out of a `&` parameter before this stage runs, and a `->` parameter is declined at the
     * site below - and `movesLocal` is the belt that says so in this pass rather than in two others.
     */
    bool clonableKind(Value::Kind kind) {
        switch(kind) {
            case Value::Alloc: case Value::LoadPlace: case Value::Init: case Value::Assign:
            // An array literal, which is one instruction rather than one per element - see
            // InstAggregate. Leaving it out made every function holding a literal un-inlinable,
            // which is how `Array.escaping` stopped folding to its constant.
            case Value::Aggregate:
            case Value::Borrow: case Value::Copy: case Value::Move:
            case Value::Drop: case Value::Swap: case Value::Exchange:
            case Value::Address:
            case Value::TypeMetric: case Value::Symbol:
            case Value::Cast: case Value::Bitcast: case Value::Neg: case Value::Not:
            case Value::Sqrt: case Value::Abs: case Value::Fma:
            case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
            case Value::Shl: case Value::Shr: case Value::Sar:
            case Value::And: case Value::Or: case Value::Xor: case Value::Cmp:
            // A callee this stage already if-converted. `settle` runs the whole round on a callee
            // before a site is judged against it, so a body reaching here can hold one - and a
            // select is an ordinary pure computation with no decision copied along with it.
            case Value::Select:
            /*
             * The vector kinds - Implementation-Vector.md §3.4's third item.
             *
             * Admitted so that a vector body can be inlined at all, and each costs one instruction
             * below like every other pure computation. Costing one by its *byte width* would make
             * every kernel look expensive and none of them would inline - which is the opposite of
             * what a vector kernel wants, since they are small and hot and inlining them is most of
             * the benefit.
             */
            case Value::VecSplat: case Value::VecLane: case Value::VecWithLane:
            case Value::VecShuffle: case Value::VecReduce:
            case Value::Call: case Value::Native: case Value::CallDyn:
                return true;
            default:
                return false;
        }
    }

    /*
     * What one instruction costs the caller, in instructions it will eventually hold.
     *
     * One, for everything whose lower form is itself. The three ownership instructions admitted to
     * `clonableKind` are the exception, and pricing them at one would be the same dishonesty §26
     * found in the resolver's form: `dischargeOwnership` runs *after* this pass now, so a `Drop` a
     * candidate holds is a load and up to three calls by the time anything emits it, and a `Swap` is
     * an allocation and six instructions. A body of four drops would be judged a body of four.
     *
     * The counts are read off `opt_discharge.cpp` rather than guessed, and each is what that pass
     * emits for the shape in hand: an empty drop is elided there and so is free here, a reclaim
     * naming the same function as the drop is one traversal serving both, and `releaseStorage` is
     * the address, the cast and the call to `freeHeap`.
     *
     * An over-estimate is the safe direction for a budget - it declines a copy rather than making
     * one - which is why the storage half is priced at its cast-bearing maximum.
     */
    U32 dischargedSize(Value& instruction) {
        switch(instruction.kind) {
            case Value::Drop: {
                auto& drop = (InstDrop&)instruction;
                if(drop.isEmpty()) return 0;

                U32 size = 1;
                if(drop.drop) size++;
                if(drop.reclaim && drop.reclaim != drop.drop) size++;
                if(drop.releaseStorage) size += 3;

                return size;
            }

            // The temporary, three relocations and three writes - and the temporary is not
            // removable, which dischargeSwap explains.
            case Value::Swap:
                return 7;

            // A relocation out and a write in, with the read costing a load or an allocation and a
            // move depending on whether the result is a register.
            case Value::Exchange:
                return 3;

            default:
                return 1;
        }
    }

    /*
     * Whether an instruction of this kind is a call at run time, which is what decides whether the
     * function containing it needs a frame - see `InlinePolicy::leafCaller`.
     *
     * `Drop` is on the list and is the one that does not look like a call: it runs a teardown, which
     * is a function, and a caller holding one is no more a leaf than one holding a `call`. `Native`
     * is not on it - a host node lowers to an instruction on this target and to an operator on the
     * other - and neither is anything else in `clonableKind`, all of which are computation.
     */
    static bool performsCall(Value::Kind kind) {
        switch(kind) {
            case Value::Call: case Value::CallDyn: case Value::GenCall: case Value::Drop:
                return true;
            default:
                return false;
        }
    }

    // The four ways a block can end that this pass knows how to graft. Everything else - a block
    // with no terminator at all, which the resolver only leaves behind on an error path - declines
    // the whole callee, since a body with a way out this does not reproduce is one whose copy would
    // simply fall off the end.
    //
    // `Unreachable` is one of them, and every check in the program depends on its being one:
    // `checkCondition` is the function every bounds test calls, `endNonReturningBlocks` ends its
    // abort arm with one before this pass runs, and refusing it here would leave every subscript in
    // the program paying a call. Copying it is the one case with nothing to reproduce - it makes no
    // edge and reads nothing.
    bool clonableTerminator(Value::Kind kind) {
        return kind == Value::Ret || kind == Value::Jmp || kind == Value::Je ||
               kind == Value::Unreachable;
    }

    /*
     * One callee, checked once and described for every call site of it.
     *
     * Everything here is a property of the *declaration*, which is what makes it worth computing
     * once: whether it can be inlined at all does not depend on who is calling, and only the budget
     * below does.
     */
    /*
     * Whether the body relocates *out of* this local.
     *
     * The belt `clonableKind` names for admitting `Move`. A move out of a slot that stays the
     * callee's own is renamed by the value map like anything else; a move out of one this graft
     * re-roots at the caller's storage would empty a place the caller's ownership state was not
     * told about, and no later pass would notice - a relocated aggregate and a live one are the
     * same bytes.
     *
     * Three departure points are asked about, because each names a place: `Move` is the relocation,
     * `Exchange` writes a new value over one it takes out, and `Drop` ends the value that is there.
     * None is reachable today for a re-rooted parameter - the borrow check refuses the `&` case and
     * `describe` refuses the sink case - so this is the statement that they stay unreachable, made
     * where the copy happens rather than in the two passes that currently imply it.
     *
     * `Drop` joined the list with `clonableKind`'s other two admissions, and it closes the hole the
     * `->` walk below names: that walk says a body owing a drop of its sink parameter "is refused by
     * `clonableKind` before this", which stopped being true the moment `Drop` was admitted.
     *
     * It asks a narrower question than the other two, and the difference is the whole of why a
     * teardown can still be inlined. **Only a drop of the slot itself counts.** A drop of a
     * *projection* of it is a member's teardown, which is what every derived reclaim is made of:
     * `reclaim$Tree` drops `value.left` and `value.right`, and re-rooting those at the caller's
     * place is the statement the site already made - `drop p reclaim f` says the value at `p` is
     * `f`'s now, so a copy of `f` rooted at `p` is that with the call boundary taken out. Matching
     * the root and ignoring the path refused every teardown in the program, which measured as a 27%
     * regression on `test/bench/programs/Tree.yana` and nothing else.
     *
     * `Swap` is deliberately not here at all: it names two places and relocates out of neither, so a
     * re-rooted parameter on either side is left holding a value of the same type.
     */
    bool movesLocal(Candidate& candidate, U32 local) {
        auto rootedHere = [&](const Place& place) {
            return place.root == PlaceRoot::Local && place.local == local;
        };

        // `const_cast` for the same reason clonePlace does it: reading a projection list's length
        // is a non-const call on the list, and a place arrives here by const reference.
        auto isLocalItself = [&](const Place& place) {
            return rootedHere(place) && const_cast<Place&>(place).projections.size() == 0;
        };

        for(auto blockPointer: candidate.blocks) {
            auto block = opt.local[blockPointer];

            for(auto pointer: block->instructions(opt.local)) {
                auto& instruction = *opt.local[pointer];

                if(instruction.kind == Value::Move &&
                   rootedHere(((InstMove&)instruction).place)) return true;

                if(instruction.kind == Value::Exchange &&
                   rootedHere(((InstExchange&)instruction).place)) return true;

                if(instruction.kind == Value::Drop &&
                   isLocalItself(((InstDrop&)instruction).place)) return true;
            }
        }

        return false;
    }

    // Whether any place in the body is rooted in this local, which is the difference between a slot
    // that is storage and a slot that is only a name.
    bool namesLocal(Candidate& candidate, U32 local) {
        auto found = false;

        auto visit = [&](Value& instruction) {
            eachPlace(instruction, [&](const Place& place) {
                if(place.root == PlaceRoot::Local && place.local == local) found = true;
            });
        };

        for(auto blockPointer: candidate.blocks) {
            auto block = opt.local[blockPointer];

            for(auto pointer: block->instructions(opt.local)) visit(*opt.local[pointer]);
            if(block->terminator()) visit(*opt.local[block->terminator()]);
        }

        return found;
    }

    /*
     * Whether this body is an accessor: a guarded read of what the call handed it, and nothing else.
     *
     * `policy.accessor` is what the answer is worth and says why. This is the shape, and every clause
     * is one of the four things that make the copy pay downstream rather than a description of a
     * small function:
     *
     *  - **one block**, so the whole body becomes the tail of the caller's own block. That is the
     *    same argument `blockCost` makes and it is stricter here on purpose: what the caller has to
     *    be able to do with the graft is fold it, and a value behind a branch is where forwarding,
     *    hoisting and discharge all stop answering. A checked accessor is still one block, because a
     *    check is a call and not a branch - see `isCheckCall`;
     *  - **a register result**, because a memory-typed one is `memoryResult`'s case and priced there.
     *    What that returns is an allocation the caller made; what this returns is a value read out of
     *    storage the caller already had;
     *  - **nothing but computation, reads and its own check.** A store, an allocation or any other
     *    call is a body that *does* something, and the whole claim here is that the callee does
     *    nothing the caller could not have written inline. `Borrow` and `Address` are admitted beside
     *    the pure kinds because they are how a place is reached rather than something done to one -
     *    an address computation is exactly what this is meant to let across;
     *  - **every place rooted in a parameter.** This is what separates an accessor from a small pure
     *    function that happens to load: `freeListHead` reads a global and is a read of the program's
     *    state, `length(self)` reads its receiver and is a projection of the caller's own. Only the
     *    second one becomes an address the caller can hoist, because only the second one is rooted in
     *    something the caller named.
     *
     * A load is required rather than merely permitted, which is the difference between this and "any
     * small pure body". `byteSpan` is nine instructions of pointer arithmetic and no load at all, and
     * inlining it is worth what any nine instructions are worth: the case for the bonus is that the
     * *address* the load used crosses the boundary, and there is no address where there is no load.
     */
    bool isAccessorBody(Candidate& candidate) {
        if(!candidate.isStraightLine()) return false;
        if(candidate.resultLocal != Candidate::kNone) return false;

        // One block whose every `ret` returns a value, which for one block is its terminator - and
        // that terminator is a `ret` rather than needing to be checked for one, since `describe` found
        // a return among these blocks and an entry nothing jumps back into.
        auto block = opt.local[candidate.blocks[0]];
        auto& terminator = *opt.local[block->terminator()];
        if(terminator.kind != Value::Ret || !((InstRet&)terminator).value) return false;

        auto loads = false;

        for(auto pointer: block->instructions(opt.local)) {
            auto& instruction = *opt.local[pointer];

            switch(instruction.kind) {
                case Value::LoadPlace:
                    loads = true;
                    break;

                // The two ways a place is named as a value rather than acted on.
                case Value::Borrow:
                case Value::Address:
                    break;

                case Value::Call:
                    if(!isCheckCall(opt, ((InstCall&)instruction).callee)) return false;
                    break;

                default:
                    if(!isPureValue(instruction)) return false;
                    break;
            }

            auto rooted = true;
            eachPlace(instruction, [&](const Place& place) {
                switch(place.root) {
                    // A borrow or a raw pointer is an address this body was handed or computed out of
                    // one it was handed, since nothing above it wrote either.
                    case PlaceRoot::Borrow:
                    case PlaceRoot::Pointer:
                        break;

                    // A local is the receiver's storage where the parameter is re-rooted, and the
                    // callee's own frame otherwise. Nothing above writes such a slot, so the second
                    // case is a read of storage nothing filled rather than a shape to price.
                    case PlaceRoot::Local:
                        if(!rerootedParameter(candidate, place.local)) rooted = false;
                        break;

                    case PlaceRoot::Global:
                        rooted = false;
                        break;
                }
            });

            if(!rooted) return false;
        }

        return loads;
    }

    /*
     * The callee's reachable blocks, in reverse postorder - see `Candidate::blocks` for why that is
     * the order and not the block list's own.
     *
     * Iterative for the reason Tarjan above is, and it carries the same two-item frame: which block,
     * and which of its two successors comes next.
     */
    void orderBlocks(Function& callee, SmallArray<ModulePtr<Block>, 12>& order) {
        ScratchSet seen(opt.sets, callee.blocks.size());

        // The walk's own three, emptied rather than built: this runs once per call site the
        // inliner considers, and most of those it goes on to decline.
        postorder.clear();
        pending.clear();
        next.clear();

        seen->set(0, true);
        pending.push(callee.blocks.get(opt.local, 0));
        next.push(0);

        while(pending.size()) {
            auto pointer = pending[pending.size() - 1];
            auto block = opt.local[pointer];

            if(next[next.size() - 1] < 2) {
                auto successor = block->successor(next[next.size() - 1]++);
                if(!successor) continue;

                auto target = opt.local[successor]->index;
                if((*seen)[target]) continue;

                seen->set(target, true);
                pending.push(successor);
                next.push(0);
                continue;
            }

            postorder.push(pointer);
            pending.pop();
            next.pop();
        }

        for(Size i = postorder.size(); i-- > 0;) order.push(postorder[i]);
    }

    // Where orderBlocks works - see there. Members rather than locals because there is one call
    // of it per call site examined, per round.
    Array<ModulePtr<Block>> postorder;
    Array<ModulePtr<Block>> pending;
    Array<Size> next;

    // Whether this callee local is one the clone never gives storage to, because every place rooted
    // in it becomes a place rooted in the caller instead. Both re-rooted bindings, since the two
    // differ in what they rewrite *to* and not in whether they rewrite.
    static bool rerootedParameter(Candidate& candidate, U32 local) {
        for(auto& parameter: candidate.parameters) {
            if(parameter.binding != Binding::Value && parameter.local == local) return true;
        }

        return false;
    }

    // The function being inlined into, which several of the rules below are about rather than about
    // the callee. Spelled the same way the self-call refusals spell it.
    ModulePtr<Function> currentFunction() const {
        return (ModulePtr<Function>)(opt.function - opt.local);
    }

    /*
     * The one recursive callee that is taken: a *different* member of the caller's own cycle.
     *
     * The header's rule is that a recursive body copied into a caller carries its recursive call
     * with it, so the copy is unrolling. That is true of a *self*-call and it is not true of a mutual
     * pair. `A` calls `B` calls `A`: copying `B` into `A` leaves `A` calling `A`, which is one
     * function where there were two and a call per level of the structure where there were two. The
     * cycle is not unrolled by it - it is *collapsed*, and the body that comes out is the shape a
     * self-recursive function would have been written as in the first place.
     *
     * What made this worth relaxing is that the compiler writes such pairs itself. A recursive data
     * type's derived teardown is one function per type in the cycle - `reclaim$Tree` calls
     * `reclaim$Maybe(Tree)` calls `reclaim$Tree` - so `test/bench/programs/Tree.yana` spent four
     * frames per node where `llc -Os` spends one, and the refusal was the cycle in the call graph
     * rather than anything about the size. §30.5 of test/bench/findings.md is the measurement.
     *
     * **One level, and it takes two bounds to say so.**
     *
     * Every site of the *same* callee is taken, because taking all of them is what finishes the
     * collapse: `reclaim$Tree` calls `reclaim$Maybe(Tree)` once per child, and leaving the second a
     * call leaves half the structure walked two frames deep. A *second* member of the cycle is
     * refused, which is what stops a cycle of `k` from becoming `k` bodies each holding all `k`.
     *
     * And a site this pass wrote is refused whatever it names, which is the bound that makes the
     * thing terminate rather than the one that keeps it small. Every other rule here is about which
     * bodies are involved, and neither of them is enough on its own: a callee that also calls itself
     * puts a fresh site of itself into its caller with every copy, and a caller that has already
     * collapsed is a *callee* full of self-calls for the next one to copy. Both are the second level
     * of an unrolling, and both are unbounded. `collapsed` is the set of sites one level produced,
     * and refusing them is the definition of "one".
     */
    bool collapsesCycle(ModulePtr<Function> pointer) {
        auto caller = currentFunction();
        if(pointer == caller) return false;
        if(site && collapsed.get(U32(site))) return false;

        auto callerCycle = cycle.getValue(U32(caller));
        auto calleeCycle = cycle.getValue(U32(pointer));
        if(!callerCycle || !calleeCycle) return false;
        if(callerCycle.unwrap() != calleeCycle.unwrap()) return false;

        auto taken = absorbed.getValue(U32(caller));
        return !taken || taken.unwrap() == U32(pointer);
    }

    /*
     * One callee, checked once and described for whatever site is about to copy it.
     *
     * `sink` is set where the site is a `drop` rather than a call. The one thing that changes is the
     * convention rule below: a `->` parameter is ownership transferred at the site, which is exactly
     * what a `drop` performs and never what a call does - see `inlineTeardown`.
     */
    Maybe<Candidate> describe(ModulePtr<Function> pointer, bool sink = false, bool env = false) {
        auto callee = opt.local[pointer];

        // `@noinline`, which is a directive rather than a weight: declining to inline is always
        // possible, so this is the one input to the decision that nothing below can outvote.
        if(callee->noInline) return Nothing();

        /*
         * `takesEnv` is the code-word convention - the environment arrives in front of the declared
         * arguments, whatever the signature says - and it is refused everywhere the site cannot
         * supply one. `env` is set where the site can: `inlineDynamicCall` resolved the code word
         * *and* the environment beside it out of one function value, so the two together are the
         * ordinary argument list this pass already knows how to bind.
         *
         * The relaxation is deliberately not "a direct call to a lifted lambda is fine". No such
         * call exists in this IR - see opt_closure.cpp - and none is created here either: what the
         * splice leaves behind is the body, not a call to it, so the convention never has to be
         * spelled by a target that does not have it.
         */
        if(callee->signature || callee->intrinsic || callee->gen) return Nothing();
        if(callee->takesEnv && !env) return Nothing();
        if(callee->blocks.isEmpty()) return Nothing();

        /*
         * A function something holds the address of, which for a call site means there is a way of
         * reaching it that no declaration at the site describes.
         *
         * Not asked on the teardown path, where the question is circular: `addressTaken` counts a
         * `Drop` naming a teardown as an address held, so every teardown answers yes and the one
         * holding the address is the site asking. Copying the body changes nothing about the other
         * ways of reaching it - a witness slot, a descriptor - since the function itself stays, and
         * whether anything still needs it is `markProgramReachable`'s answer at the end of the stage.
         */
        if(!sink && !env && taken.get(U32(pointer))) return Nothing();

        // A body that can reach itself, which is unrolling rather than inlining - see the header and
        // `collapsesCycle`, which is the one shape of it that is neither.
        if(recursive.get(U32(pointer)) && !collapsesCycle(pointer)) return Nothing();

        // And the body as anything would emit it rather than as the resolver wrote it - see
        // `settleCallee`, which is the same statement `settle` makes about a caller.
        settleCallee(pointer, *callee);

        Candidate candidate;
        candidate.pointer = pointer;
        candidate.callee = callee;
        orderBlocks(*callee, candidate.blocks);

        if(candidate.blocks.size() > policy.maxBlocks) return Nothing();

        /*
         * The entry block has to be one the caller's own block can *become*, since that is what the
         * graft does with it: what was in front of the call stays in front of the body. A phi there,
         * or an edge back into it, would mean the entry is a join - and a join cannot be the tail of
         * a block that already has instructions in it. Nothing the resolver emits is one, because a
         * loop's test is always a block of its own.
         */
        auto entry = opt.local[candidate.blocks[0]];
        if(entry->phiCount() != 0 || entry->predecessorCount() != 0) return Nothing();

        for(auto blockPointer: candidate.blocks) {
            auto block = opt.local[blockPointer];
            if(!block->terminator()) return Nothing();

            auto kind = opt.local[block->terminator()]->kind;
            if(!clonableTerminator(kind)) return Nothing();
            if(kind == Value::Ret) candidate.returns.push(blockPointer);

            for(auto instructionPointer: block->instructions(opt.local)) {
                auto& instruction = *opt.local[instructionPointer];
                if(!clonableKind(instruction.kind)) return Nothing();

                // Whether the copy brings a call in with it - see `InlinePolicy::leafCaller`, which
                // is the one term that is about the caller's frame rather than about either body.
                if(performsCall(instruction.kind)) candidate.callFree = false;

                // And how many of them are indirect, which is what `policy.chainedCall` prices.
                // Counted here rather than walked for, because this is already the walk of every
                // instruction.
                if(instruction.kind == Value::CallDyn) candidate.dynamicCalls++;

                candidate.size += dischargedSize(instruction);
            }

            // A phi is an instruction the caller pays for like any other, and on a managed target it
            // is a variable and an assignment on every edge into the join.
            candidate.size += U32(block->phiCount());
        }

        // A callee that never returns. The continuation would be unreachable and the call's result
        // would have nothing to be, which is a correct program and not one worth building here.
        if(candidate.returns.isEmpty()) return Nothing();

        /*
         * Which parameter each local belongs to, which is the map the parameter walk below needs and
         * the local table does not have: a local records the *value* its storage is, so the question
         * "does this argument have a local" is answered by looking for it.
         */
        for(Size i = 0; i < callee->args.size(); i++) {
            auto argPointer = callee->args.get(opt.local, i);
            auto arg = opt.local[argPointer];

            /*
             * A sink transfers ownership into the callee, which is a decision taken at the site
             * rather than in the body and does not survive being spliced away.
             *
             * A `drop` site's sink does, and is the whole of what it is: `drop p reclaim f` says
             * that the value at `p` is `f`'s now, so a copy of `f` re-rooted at `p` is the same
             * statement with the call boundary taken out. Which is why this is the only relaxation
             * the teardown path asks for.
             *
             * A `return` parameter used to be refused beside it, on the reading that the caller's
             * loan had been sized against the *summary* rather than against the body - so splicing
             * the body in would leave a loan measured against a call that no longer exists. It is
             * the other way round: the borrow check has already run and the loan it computed is
             * already in this function's ownership result, which nothing after this stage recomputes.
             * A summary-sized extent is *wider* than the body's own would have been, so what a
             * splice can do to it is make it conservative, and conservative is the direction a loan
             * is allowed to be wrong in. §4 measured this and found zero `.run.expect` failures; what
             * it wanted before removing the guard was this argument rather than that measurement.
             */
            /*
             * A `->` parameter is what the site relocated into, and the relocation stays where it
             * was: the caller's `InstMove` is in front of the call and survives the splice, so what
             * the graft has to preserve is only what the *body* then did with the value.
             *
             * Which is one of two things, because the callee owns the slot. Either it hands it on -
             * a `Move`, an `init` from the parameter, a `ret` - which the value map renames like any
             * other operand; or it owes a drop, and a body containing one is refused by
             * `clonableKind` before this. The third shape, a body that reaches the parameter through
             * a *place* and relocates out of it, is the one that would empty storage the caller
             * still names, and `movesLocal` refuses it below.
             *
             * The teardown path's relaxation is unchanged and is a different statement:
             * `drop p reclaim f` says the value at `p` is `f`'s now, so a copy of `f` re-rooted at
             * `p` is that same statement with the call boundary taken out.
             */
            if(arg->convention == ast::BindType::Sink && sink && callee->args.size() != 1) {
                return Nothing();
            }

            Parameter parameter;
            parameter.binding = Binding::Value;
            parameter.arg = (ModulePtr<Value>)argPointer;
            // Whether the body reaches this parameter at all - see `Parameter::used`, which is why
            // this is the value's own use list rather than a walk for the `calldyn` that reads it.
            parameter.used = arg->useCount() != 0;

            for(U32 local = 0; local < callee->localCount(); local++) {
                auto slot = callee->localAt(opt.local, local);
                if(slot.value != (ModulePtr<Value>)argPointer) continue;

                // A `&` parameter's slot is the caller's storage, which the rewrite below turns
                // into a place rooted in the caller's borrow.
                if(slot.borrowed) {
                    parameter.binding = Binding::Borrowed;
                    parameter.local = local;
                    break;
                }

                /*
                 * Any other parameter with a slot, and the question is whether the body ever reaches
                 * the parameter *through* it.
                 *
                 * Where it does not, the slot is bookkeeping and the parameter is an ordinary SSA
                 * value that `mapValue` already substitutes correctly. Telling the two apart matters
                 * more than it sounds: a scalar value parameter gets a slot too, so assuming a slot
                 * meant memory refused every function with one - which is most of Core's operators.
                 * `+=(Int)` is four instructions over a mutable borrow, the single most rewarding
                 * shape there is on a managed target, and it was being declined on its *second*
                 * parameter for having storage nothing reads.
                 *
                 * Where it does, the slot is an address, and *whose* address decides whether there
                 * is anything to rewrite. A memory type arrives as the caller's own storage, so the
                 * callee's places are the caller's places with a prefix missing and `Binding::Memory`
                 * supplies it. A scalar with a slot the body reaches through is not that: the
                 * storage is the callee's own, materialized out of a register on entry, and this
                 * frame has nothing to re-root it at.
                 */
                if(namesLocal(candidate, local)) {
                    if(!arg->type || !isMemoryType(opt.global, arg->type)) return Nothing();

                    parameter.binding = Binding::Memory;
                    parameter.local = local;
                }

                break;
            }

            candidate.parameters.push(parameter);
        }

        for(U32 local = 0; local < callee->localCount(); local++) {
            auto slot = callee->localAt(opt.local, local);

            /*
             * A **closure environment** used to be refused here, and the rule was one of placement
             * rather than of copying: the storage is the function value's rather than this frame's -
             * see Local::closureEnv - so the flag travels with the local, which the graft now does.
             * `InstAlloc::storage` and `releasedHere` already travelled, which is the same statement
             * about the same allocation; the flag was the half of it that was being dropped.
             *
             * What the refusal cost is every function that builds a closure, which is every adaptor
             * in the program: `mapped` is a closure and a call and nothing else.
             */
            /*
             * A local of a type owing a teardown used to be refused outright here, exempting only a
             * parameter whose storage is the caller's. The rule was a *belt*, and its own comment
             * said so: a body with such a local either owes a `Drop`, which `clonableKind` refuses,
             * or hands ownership on through a `Move` or a `ret`, which it also refused - so nothing
             * reached here that had not already been declined, and this was the line that would
             * catch a kind admitted to `clonableKind` later.
             *
             * A kind was admitted later, and it is the one the belt was insurance against. So the
             * question it was standing in for has to be answered rather than re-tightened, and the
             * answer is the one clonableKind gives: a graft copies a whole body, so the callee's own
             * local becomes a fresh local of this frame's and the relocation out of it is renamed
             * with everything else. What is *not* renamed is a re-rooted parameter, which is the
             * case that is genuinely different and the one thing left here.
             */
            if(rerootedParameter(candidate, local)) {
                // The one thing a re-rooted slot may not do - see movesLocal.
                if(movesLocal(candidate, local)) return Nothing();
            }
        }

        auto& first = (InstRet&)*opt.local[opt.local[candidate.returns[0]]->terminator()];
        auto returnsValue = first.value != nullptr;

        for(auto blockPointer: candidate.returns) {
            auto& ret = (InstRet&)*opt.local[opt.local[blockPointer]->terminator()];

            // Some paths returning a value and others returning none is not a shape the phi below
            // has an answer for, and not one a well-typed body produces either.
            if((ret.value != nullptr) != returnsValue) return Nothing();
            if(!ret.value) continue;

            auto type = opt.local[ret.value]->type;

            /*
             * A result the target holds in memory is returned out of storage rather than in a
             * register, and the caller allocated that storage for the call. So the callee's local
             * has to *become* the caller's, which needs the returned value to be an allocation this
             * body made - a returned parameter or global has no such correspondence.
             *
             * That rename is only available where there is one `ret`. Two of them are two callee
             * locals that would both have to be the caller's one slot, and a slot holds the single
             * value its storage came from - so the phi that answers this for a register result has
             * no counterpart here.
             *
             * What answers it instead is the thing a `ret` of a memory value already *is*: lowering
             * emits a copy of the bytes into the return place the caller passed - see lower_call.cpp
             * - so the graft writes each returning path's value into one slot of the caller's and
             * hands that slot back. `copiedResult` is that shape, and the only thing it asks of a
             * returned value is that there is one, since any value of the type can be copied.
             *
             * The rename is kept for the single-`ret` case rather than folded into this, because it
             * is strictly better: the callee builds the result in the caller's storage directly and
             * no copy is emitted at all. This is what an adaptor's `Outcome` needs, and every one of
             * them has a `ret` per arm.
             */
            if(type && isMemoryType(opt.global, type)) {
                if(candidate.returns.size() != 1) {
                    candidate.copiedResult = true;
                    continue;
                }

                auto returned = opt.local[ret.value];
                if(returned->kind != Value::Alloc) return Nothing();

                candidate.resultLocal = ((InstAlloc&)*returned).local;
                if(candidate.resultLocal >= callee->localCount()) return Nothing();
            }
        }

        // Last, because it asks which parameters were re-rooted and which local a memory result came
        // out of, and both of those are what the two walks above just decided.
        candidate.accessor = isAccessorBody(candidate);

        return Just(::move(candidate));
    }

    // A value the caller handed over as a literal, which is what every rule below means by a
    // constant argument. `isConstant` is the same question the IR already answers - see inst.def -
    // and it is deliberately not `constantValueOf`: this asks whether the folder will have something
    // to work with, not what the number is.
    static bool isLiteral(const Value& value) { return isConstant(value); }

    /*
     * Whether one value in the callee is already settled by this call site: a literal, an argument
     * the caller passed a literal for, or a computation over nothing else.
     *
     * The kinds admitted are the ones opt_fold.cpp can actually answer, which is why this is not
     * `isPureValue` - a `Symbol` and a `TypeMetric` are pure and have no operands, so that predicate
     * would call the address of a function "decided" and it is not a number anything folds against.
     *
     * `decided` is seeded with the arguments and then memoises the walk, which also closes the one
     * cycle a value graph can have: a phi is not among the kinds below, so a value reached twice is
     * a shared subexpression rather than a loop, and the entry written before the recursion is what
     * makes a body that manages one answer `false` instead of not answering.
     */
    bool decidedAtCall(ModulePtr<Value> value, HashMap<U32, U8>& decided) {
        if(!value) return false;
        if(auto found = decided.getValue(U32(value))) return found.unwrap() != 0;

        *decided.add(U32(value)).value = 0;

        auto& instruction = *opt.local[value];

        // A literal is settled with nothing to walk. Asked here rather than as arms of the switch
        // below, so that what counts as one stays the single answer `isLiteral` gives.
        auto answer = isLiteral(instruction);

        if(!answer) switch(instruction.kind) {
            case Value::Cast: case Value::Bitcast: case Value::Neg: case Value::Not:
            case Value::Sqrt: case Value::Abs: case Value::Fma:
            case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
            case Value::Shl: case Value::Shr: case Value::Sar:
            case Value::And: case Value::Or: case Value::Xor: case Value::Cmp:
            // A select of decided arms on a decided condition is decided, which is `foldSelect`'s
            // rule read forwards: the site knows which arm survives, so a branch below it on the
            // result is one that does not survive the call either.
            case Value::Select:
                answer = true;
                eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
                    if(!decidedAtCall(operand, decided)) answer = false;
                });
                break;
            default:
                break;
        }

        *decided.add(U32(value)).value = answer ? 1 : 0;
        return answer;
    }

    /*
     * Whether no branch in the callee survives this call site.
     *
     * This is the term `blockCost` was missing rather than a second opinion about it. What that cost
     * prices is the graft: blocks spliced into a caller that the passes below then have to live with,
     * and on a managed target one more join for codegen/js/flow.cpp to recover a structured form of.
     * But a `je` whose condition the caller decided is not a join that survives - opt_branch.cpp
     * turns it into a `jmp` on the very next round, deletes the arm nothing reaches and merges what
     * is left - so where *every* branch is decided the graft leaves one block, and charging for the
     * others is charging for blocks that are gone before anything can be asked about them.
     *
     * `Truth.yana` is what this is measured on, and it is the case where the two targets had come
     * apart: `fromInt(7)` is two instructions in four blocks, which native inlined and JS refused,
     * because 3 blocks at a managed `blockCost` of 3 is more than the whole base budget. So native
     * folded `main` to one constant and JS emitted eight calls to functions whose arguments it
     * already knew - bigger and slower, on the target that pays most for both.
     *
     * A callee with no branches at all answers true and is charged nothing, which is what it was
     * already paying: it has one block, and a chain of `jmp`s is one block after `mergeBlocks`.
     */
    bool decidesEveryBranch(Candidate& candidate) {
        auto& decided = decidedScratch;
        decided.reset();

        for(Size i = 0; i < candidate.callee->args.size(); i++) {
            // A site that passes nothing decides nothing, which is every `drop`: its one parameter
            // is a place rather than a value, and a place is not something the folder answers with.
            auto argument = i < candidate.arguments.size() ? candidate.arguments[i] : nullptr;
            auto known = argument && isLiteral(*opt.local[argument]);

            *decided.add(U32((ModulePtr<Value>)candidate.callee->args.get(opt.local, i))).value =
                known ? 1 : 0;
        }

        for(auto blockPointer: candidate.blocks) {
            auto block = opt.local[blockPointer];
            if(!block->terminator()) return false;
            if(opt.local[block->terminator()]->kind != Value::Je) continue;

            if(!decidedAtCall(((InstJe&)*opt.local[block->terminator()]).cond, decided)) return false;
        }

        return true;
    }

    /*
     * Whether the callee's result is a reference a managed target has to build an object for.
     *
     * Approximate on purpose, and in the cheap direction: what the target actually asks is
     * `codegen/js/type.cpp`'s isJsObject, which this stage has no business importing - it is a
     * question about one backend's representation and the Repr is the part of it that lives here.
     * A pointee that is a memory type with an object layout is the case where the answer is "no
     * object to remove", and it is the only one that has to be right; a case this gets wrong at
     * worst declines a bonus.
     */
    bool returnsReifiedReference(Function& callee) {
        auto returned = callee.returnType;
        if(!returned || opt.global[returned]->kind != Type::Borrow) return false;

        auto pointee = ((BorrowType*)opt.global[returned])->to;
        if(!pointee || !isMemoryType(opt.global, pointee)) return true;

        auto kind = opt.global[pointee]->kind;
        if(kind == Type::Fun || kind == Type::String) return true;

        auto& repr = opt.repr.of(pointee);
        if(repr.opaque) return false;

        return repr.scalarBits != 0 || repr.isNicheFolded();
    }

    /*
     * Whether the caller performs exactly one call, which is the site's half of `policy.leafCaller`.
     *
     * The site being judged is that call - `inlineCall` reached this from an instruction it found in
     * this function - so "one" is what makes the copy leave none. Counted rather than cached because
     * the answer changes under this pass's own grafts, and stopped at two because that is the whole
     * of what the question needs.
     */
    bool callerHasOneCall() {
        U32 calls = 0;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(auto pointer: block->instructions(opt.local)) {
                if(!performsCall(opt.local[pointer]->kind)) continue;
                if(++calls > 1) return false;
            }
        }

        return calls == 1;
    }

    /*
     * What the closure term is worth at this site, summed over the parameters that earn it.
     *
     * Both halves are asked in the cheap order: the callee has to use the parameter, which is a flag
     * `describe` already set, and only then is the argument worth the walk that says which lambda it
     * is.
     *
     * Read twice - once against the ceiling and once against the limit - which is why it is a
     * function rather than a term computed inside `worthInlining`'s parameter walk. Nothing here
     * depends on the caller's state, so the two readings are the same number.
     */
    U32 closureBonus(Candidate& candidate) {
        if(!policy.closureArgument) return 0;

        /*
         * The parameters an argument was passed for, which is not all of them: a teardown site binds
         * `parameters[0]` to the *place* being dropped and passes nothing at all, so the two lists
         * have different lengths there and pairing them by index reads past the end of one.
         *
         * A site with no argument in a position has no lambda in it either, so stopping at the
         * shorter list is the answer rather than a way of avoiding the question.
         */
        U32 bonus = 0;
        for(Size i = 0; i < min(candidate.parameters.size(), candidate.arguments.size()); i++) {
            if(!candidate.parameters[i].used) continue;
            if(!knownCallee(candidate.arguments[i], false)) continue;

            bonus += policy.closureArgument;
        }

        return bonus;
    }

    /*
     * Whether the environment this site hands over holds a function value *this* frame built.
     *
     * The question the chain term rests on, and the whole of what stops it firing in the frame where
     * the copy would be wasted. `vectors` hands the chunk walker an environment holding a borrow of
     * the loop body it was itself passed - so copying the walker into `vectors` brings two `calldyn`
     * in and resolves neither, since the body is that frame's *argument* and its code word is
     * whatever the caller wrote. The identical site inside the caller, once `vectors` has been
     * copied into it, hands over an environment holding a borrow of the closure the caller built,
     * and there the same copy resolves both.
     *
     * Two spellings, which are the two ways a capture is written: the function value itself, and a
     * borrow of the local holding one. `knownCallee` answers the first directly and the second
     * through the borrowed local's own storage, which is the value the closure was built in.
     */
    bool environmentHoldsClosure(Place& environment) {
        if(environment.root != PlaceRoot::Local) return false;
        if(environment.projections.isNotEmpty()) return false;
        if(environment.local >= opt.function->localCount()) return false;

        auto storage = opt.function->localAt(opt.local, environment.local).value;
        if(!storage) return false;

        for(auto user: opt.local[storage]->uses(opt.local)) {
            auto& instruction = *opt.local[user];
            if(instruction.kind != Value::Init && instruction.kind != Value::Assign) continue;

            auto written = ((InstInit&)instruction).value;
            if(!written) continue;

            if(opt.local[written]->kind == Value::Borrow) {
                auto& borrow = (InstBorrow&)*opt.local[written];
                if(borrow.place.root != PlaceRoot::Local) continue;
                if(borrow.place.projections.isNotEmpty()) continue;
                if(borrow.place.local >= opt.function->localCount()) continue;

                written = opt.function->localAt(opt.local, borrow.place.local).value;
                if(!written) continue;
            }

            if(knownCallee(written, false)) return true;
        }

        return false;
    }

    /*
     * And the same question for the link below it - see `policy.chainedCall`, whose two halves this
     * is. Read twice for the reason `closureBonus` is.
     */
    U32 chainBonus(Candidate& candidate) {
        if(!candidate.dynamic || !policy.chainedCall || !candidate.dynamicCalls) return 0;
        if(candidate.parameters.isEmpty()) return 0;
        if(!environmentHoldsClosure(candidate.parameters[0].storage)) return 0;

        return min(candidate.dynamicCalls, policy.chainedCap) * policy.chainedCall;
    }

    /*
     * Whether the copy *moves* this body rather than duplicating it.
     *
     * `soleCallSite` from the other end, and the same statement: a callee whose only way in is this
     * site leaves nothing behind, so what the ceiling is guarding against - a large body copied on
     * the strength of enough small reasons - is not what would happen. It is the last link of an
     * adaptor chain and it is the one the other two terms cannot reach: `Collections.continuation$12`
     * is `indexOf`'s own loop body at size 93, and it holds no indirect call of its own, so neither
     * the closure term nor the chain term has anything to say about it.
     *
     * Four things have to hold and each of them is what makes "only way in" true rather than likely.
     * The site is one this pass *resolved*, so the function value it goes through is one this frame
     * built. The callee is a `takesEnv` body - a lifted lambda, which no source name reaches. And
     * exactly one `symbol` instruction in the whole program names it, which is that closure: a second
     * would be another function value able to reach the same body, and a body two frames can reach is
     * one this copy duplicates rather than moves.
     *
     * The fourth is that the callee holds **no indirect call of its own**, which makes this the last
     * link and nothing else. A body that does hold one is a link in the middle, and where it should
     * be copied to is not a question about how many frames can reach it - it is `chainBonus`'s
     * question, and the answer depends on which frame built the closure the copy would resolve. Read
     * without this, the rule moved the chunk walker into `vectors`, where the loop body it then
     * calls is that frame's own argument: the copy resolved nothing and left `vectors` too large to
     * be copied into the caller that could have. Measured, that is `VecFloat` at 5.0 ms against
     * 10.6.
     *
     * `callSites` says nothing here and cannot: nothing *calls* a lifted lambda by name, so its
     * count is zero for every one of them and `soleCallSite` was already being paid on that basis.
     * The code-word census is the same question asked where the answer is - see `codeWords`.
     */
    bool movesIntoSite(Candidate& candidate) {
        // Spent where `soleCallSite` is, since it is that term's argument: zero only at
        // `InlineLevel::None`, where nothing is inlined at all and a bisection expects it.
        if(!policy.soleCallSite) return false;
        if(!candidate.dynamic || !candidate.callee->takesEnv) return false;
        if(candidate.dynamicCalls != 0) return false;

        auto words = codeWords.getValue(U32(candidate.pointer));
        return words && words.unwrap() == 1;
    }

    /*
     * Whether this call site is worth what the copy costs.
     *
     * The budget is the callee's size against a limit built from what the *call* looks like, and
     * every term is named in `InlinePolicy`. A call site that clears the ceiling is refused whatever
     * else it has going for it.
     *
     * **The ceiling is the two closure terms' too**, and those two are the only ones that raise it.
     * The rest of the table prices a body against what it contains, and a ceiling over the sum of
     * them is what stops a large body being copied on the strength of enough small reasons - see
     * `InlinePolicy::ceiling`. Neither of these is one of those: what they buy is not the call the
     * copy removes but the ones below it, and the bodies holding an indirect call over a container
     * are loops, which are large by construction. Weighed under a fixed ceiling they can therefore
     * never be spent on the callees they were written for - `Collections.continuation$1` is eleven
     * blocks and size 60 against a ceiling of 48, refused with a bonus of 56 standing unspent beside
     * it - which makes each of them a budget line that the shape it exists for cannot reach.
     *
     * **And a body that moves is not weighed against it at all** - see `movesIntoSite`. The ceiling
     * is about what a *copy* costs, and there is no copy: the callee is a lifted lambda this site is
     * the only way into, so what the program holds afterwards is the same body in one place instead
     * of two. `soleCallSite` makes the same argument about the limit and has made it all along.
     *
     * `maxBlocks` is deliberately *not* lifted with any of them. That cap prices what a graft costs
     * the passes downstream rather than what the body contains, and it is the same cost whichever
     * term paid for the copy. See §34.6 and §35 of test/bench/findings.md.
     */
    bool worthInlining(Candidate& candidate) {
        auto closure = closureBonus(candidate) + chainBonus(candidate);
        auto moves = movesIntoSite(candidate);

        if(!moves && candidate.size > policy.ceiling + closure) return false;

        auto sites = callSites.getValue(U32(candidate.pointer));
        auto count = sites ? sites.unwrap() : U32(0);

        auto limit = I64(policy.budget);
        if(moves) limit += I64(candidate.size);
        else if(count <= 1) limit += policy.soleCallSite;
        else if(count >= policy.manyCallSites) limit -= policy.manyPenalty;
        else limit -= policy.repeatedPenalty;

        U32 constants = 0;
        for(auto argument: candidate.arguments) {
            if(argument && isLiteral(*opt.local[argument])) constants++;
        }

        limit += I64(min(constants, policy.constantCap)) * policy.constantArgument;

        for(auto& parameter: candidate.parameters) {
            if(parameter.binding == Binding::Borrowed) limit += policy.mutableBorrow;
        }

        limit += closure;

        if(candidate.resultLocal != Candidate::kNone || candidate.copiedResult) {
            limit += policy.memoryResult;
        }
        if(policy.borrowResult && returnsReifiedReference(*candidate.callee)) {
            limit += policy.borrowResult;
        }

        if(candidate.accessor) limit += policy.accessor;

        // Both halves, asked in the cheap order: the callee's is a flag `describe` already set while
        // it was walking every instruction, and only a call-free one is worth the walk that counts
        // the caller's own calls.
        if(policy.leafCaller && candidate.callFree && callerHasOneCall()) limit += policy.leafCaller;

        // A recursive callee is one `describe` only admits through `collapsesCycle`, so reaching
        // here at all is the collapse - see the term, which is priced against a call per level of
        // the recursion rather than against this one.
        if(recursive.get(U32(candidate.pointer))) limit += policy.cycleCollapse;

        if(candidate.callee->inlineHint) limit += policy.requested;

        /*
         * What the graft costs, which the straight-line case does not pay because it performs none -
         * and neither does one whose every branch this call site has already decided.
         *
         * The two cheap terms first so that the walk behind the third is only ever run where its
         * answer could change something: a callee of one block is the common case and pays nothing
         * either way, and `blockCost` is zero at `InlineLevel::Size`, where a body moves rather than
         * being copied.
         *
         * **And not against a memory-typed result**, which is the case the charge was never about.
         * What it prices is the graft leaving something the caller wanted to see *behind a branch*,
         * where the caller's block-local passes stop answering. A callee handing back a memory type
         * has exactly one return block - `describe` requires it, because a slot holds the single
         * value its storage came from - so the allocation and every write into it land in one block,
         * together with the caller's own uses of them after the splice. The branching above that is
         * ordinary code the caller now contains, and `size` is already what prices ordinary code.
         *
         * This is what makes a container's `slice` inlinable. Its thirteen blocks are four clamps
         * that all reconverge before the descriptor is built, so the object it returns is exactly
         * what the caller takes apart - see Implementation-Simplification.md §21.
         */
        auto blocks = candidate.blocks.size();
        if(blocks > 1 && policy.blockCost && decidesEveryBranch(candidate)) blocks = 1;
        if(blocks > 1 && policy.blockCost && candidate.resultLocal != Candidate::kNone) blocks = 1;

        limit -= I64(blocks - 1) * policy.blockCost;

        return I64(candidate.size) <= limit;
    }

    /*
     * The clone itself.
     *
     * One arena holds the whole program - `OptContext::local` is the program's, not a module's - so
     * a type, a constant, a global and a function pointer are the same handle in the caller as in
     * the callee, and none of them needs translating. What does need translating is exactly three
     * things: a value defined in the body, a block, and a local index, which a `Place` carries by
     * number.
     */
    struct Clone {
        HashMap<U32, U32> values;
        HashMap<U32, U32> blocks;
        Array<U32> locals;
        InstList emitted;
        Block* into = nullptr;

        // Emptied rather than rebuilt: one of these is used per call site inlined, and every table
        // in it is the same shape each time - see HashMap::reset.
        void clear() {
            values.reset();
            blocks.reset();
            locals.clear();
            emitted.clear();
            into = nullptr;
        }
    };

    // The two tables the passes above reuse rather than build: what one clone translates, and which
    // of a candidate's branches its arguments decide. Both are per call site considered, which is
    // the frequency this pass runs at.
    Clone cloneScratch;
    HashMap<U32, U8> decidedScratch;

    // One callee block as it is being built in the caller: created before anything is cloned, so
    // that a branch to it has something to name, and filled in afterwards.
    struct ClonedBlock {
        ModulePtr<Block> from = nullptr;
        Block* to = nullptr;
        InstList phis;
        InstList instructions;
        Inst* terminator = nullptr;
    };

    ModulePtr<Block> mapBlock(Clone& clone, ModulePtr<Block> block) {
        auto found = clone.blocks.getValue(U32(block));
        return found ? ModulePtr<Block>(found.unwrap()) : nullptr;
    }

    /*
     * One operand, against the caller.
     *
     * A constant is copied rather than shared, and that is not tidiness. A constant belongs to no
     * block, so nothing about it is *wrong* in another function - but `mapConstant` in
     * resolve/lower.cpp materializes each one once and caches the result in a map that lives for
     * the whole program, keyed by the resolve handle. That cache is correct today because a handle
     * is only ever reached from the one function that built it, and inlining is the first thing that
     * would have made two functions share one: the second would have got the first's `LowerImm`,
     * in the first's entry block, which `validateLowerModule` reports as a value from the wrong
     * function. Copying keeps the invariant the cache rests on rather than weakening the cache.
     *
     * Everything else the body names is genuinely outside it - a global, a callee, a type - and
     * those are program-level handles that mean the same thing here.
     */
    ModulePtr<Value> mapValue(Clone& clone, ModulePtr<Value> value) {
        if(!value) return nullptr;

        if(auto found = clone.values.getValue(value)) return ModulePtr<Value>(found.unwrap());

        auto& module = *opt.module;
        auto& function = *opt.function;
        auto& constant = *opt.local[value];
        Value* copy = nullptr;

        switch(constant.kind) {
            case Value::ConstInt:
                copy = addConstant<ConstInt>(module, function, *clone.into, constant.source,
                                             constant.type, ((ConstInt&)constant).value);
                break;
            case Value::ConstFloat:
                copy = addConstant<ConstFloat>(module, function, *clone.into, constant.source,
                                               constant.type, ((ConstFloat&)constant).value);
                break;
            case Value::ConstDouble:
                copy = addConstant<ConstDouble>(module, function, *clone.into, constant.source,
                                                constant.type, ((ConstDouble&)constant).value);
                break;
            case Value::ConstString:
                copy = addConstant<ConstString>(module, function, *clone.into, constant.source,
                                                constant.type, ((ConstString&)constant).text);
                break;
            default:
                return value;
        }

        auto copied = (ModulePtr<Value>)(copy - opt.local);
        *clone.values.add(U32(value)).value = U32(copied);
        return copied;
    }

    /*
     * A place, rebuilt against the caller.
     *
     * A local root is renumbered. A root that is the `&` parameter's local becomes a *borrow* root
     * on whatever the caller passed - which is what makes `n = n + 1` in the callee into a read and
     * a write of the caller's own storage without this pass having to know what that storage is.
     *
     * A root that is a memory-typed value parameter's local becomes the caller's own place, path
     * and all, and the callee's path is appended to it below: `%self@Run.items` inside
     * `releaseRun(value.run)` is `%value@Array.run@Run.items` in the frame that called it. The
     * caller's projections are copied rather than mapped, because they are already the caller's
     * values - `mapValue` only ever has an answer about the callee's.
     */
    Place clonePlace(Clone& clone, Candidate& candidate, const Place& place) {
        Place result;
        result.root = place.root;
        result.global = place.global;
        result.local = place.local;
        result.pointer = mapValue(clone, place.pointer);

        if(place.root == PlaceRoot::Pointer && place.pointer) {
            for(auto& parameter: candidate.parameters) {
                if(parameter.binding != Binding::Pointer) continue;
                if(parameter.arg != place.pointer) continue;

                auto& storage = parameter.storage;
                result.root = storage.root;
                result.local = storage.local;
                result.global = storage.global;
                result.pointer = storage.pointer;

                for(Size p = 0; p < storage.projections.size(); p++) {
                    result.projections.push(opt.program.arena, storage.projections.get(opt.local, p));
                }

                break;
            }
        }

        if(place.root == PlaceRoot::Local) {
            auto rewritten = false;

            for(Size i = 0; i < candidate.parameters.size(); i++) {
                auto& parameter = candidate.parameters[i];
                if(parameter.binding == Binding::Value || parameter.binding == Binding::Pointer ||
                   parameter.local != place.local) continue;

                if(parameter.binding == Binding::Borrowed) {
                    result.root = PlaceRoot::Borrow;
                    result.local = 0;
                    result.pointer = candidate.arguments[i];
                } else {
                    auto& storage = parameter.storage;
                    result.root = storage.root;
                    result.local = storage.local;
                    result.global = storage.global;
                    result.pointer = storage.pointer;

                    for(Size p = 0; p < storage.projections.size(); p++) {
                        result.projections.push(opt.program.arena, storage.projections.get(opt.local, p));
                    }
                }

                rewritten = true;
                break;
            }

            if(!rewritten) result.local = clone.locals[place.local];
        }

        auto& projections = const_cast<Place&>(place).projections;
        for(Size i = 0; i < projections.size(); i++) {
            auto projection = projections.get(opt.local, i);
            projection.value = mapValue(clone, projection.value);
            result.projections.push(opt.program.arena, projection);
        }

        return result;
    }

    Inst* cloneInstruction(Clone& clone, Candidate& candidate, Block& into, Value& instruction) {
        auto& module = *opt.module;
        auto& function = *opt.function;
        auto source = instruction.source;
        auto name = instruction.name;
        auto type = instruction.type;

        auto value = [&](ModulePtr<Value> operand) {
            return mapValue(clone, operand);
        };

        auto place = [&](const Place& from) {
            return clonePlace(clone, candidate, from);
        };

        switch(instruction.kind) {
            case Value::Alloc: {
                auto& alloc = (InstAlloc&)instruction;
                auto cloned = createInst<InstAlloc>(module, function, into, source, name, type,
                                                    clone.locals[alloc.local]);

                // The escape decision travels with the allocation. A callee local that went to the
                // heap goes to the heap here, and one the callee released itself is released here -
                // this frame outlives the region the call occupied, so neither answer changes.
                cloned->storage = alloc.storage;
                cloned->releasedHere = alloc.releasedHere;
                cloned->storageFlag = value(alloc.storageFlag);
                cloned->closure = alloc.closure;

                /*
                 * And *why* it went there, where the reason outranks the analysis.
                 *
                 * `ownedElsewhere` is the target of a box, which is out of line whatever anything
                 * proved - the owner's derived `Reclaim` is what frees it, and that function is
                 * interned per type, so it has one answer for every value of that type. Dropping it
                 * here left a clone that looked like an ordinary heap allocation with no reason of
                 * its own, which was invisible while nothing re-asked the question and became a
                 * frame address handed to `freeHeap` the moment `reselectStorage` did. Box.yana is
                 * the fixture that says so, and it says it by segfaulting.
                 */
                cloned->ownedElsewhere = alloc.ownedElsewhere;

                /*
                 * How many slots, which is an *operand* and therefore has to be remapped -
                 * InstAlloc::extent, and the one field of an allocation this had been dropping.
                 *
                 * Losing it turned a run of `n` into an allocation of one, which is a silent
                 * miscompile rather than a diagnostic, and the reason nothing caught it is that
                 * every run until now got its extent from an array literal - a `ConstInt`, which
                 * constants-materialize per function and so survived being carried across by
                 * accident. `newStringOfCapacity` is the first caller to pass a computed one, and
                 * the symptom was a value the inlined body used and no body defined.
                 *
                 * `generic.cpp`'s clone has always carried it; this is the same line, in the pass
                 * that had been missing it.
                 */
                cloned->extent = value(alloc.extent);
                return (Inst*)cloned;
            }
            case Value::LoadPlace: {
                auto cloned = createInst<InstLoadPlace>(module, function, into, source, name, type,
                                                        place(((InstLoadPlace&)instruction).place));

                // The overread flag, on the same terms `InstAlloc::extent` above is carried: it is
                // a field rather than an operand, so a clone that rebuilds the instruction from its
                // constructor arguments loses it silently. Every overreading load in a real program
                // is inside `loadVectorTail`, which is small enough that the inliner always takes
                // it - so dropping it here dropped it everywhere.
                cloned->overread = ((InstLoadPlace&)instruction).overread;
                return (Inst*)cloned;
            }
            case Value::Init:
            case Value::Assign: {
                auto& write = (InstInit&)instruction;
                return (Inst*)createInst<InstInit>(module, function, into, source, name, type,
                                                   place(write.place), value(write.value),
                                                   instruction.kind);
            }
            case Value::Aggregate: {
                auto& aggregate = (InstAggregate&)instruction;
                auto cloned = createInst<InstAggregate>(module, function, into, source, name, type,
                                                        place(aggregate.place));
                cloned->constructor = aggregate.constructor;

                eachAggregateComponent(opt.local, aggregate,
                                       [&](AggregateComponent component, Size) {
                    if(component.step.value) component.step.value = value(component.step.value);

                    cloned->components.push(module.arena, AggregateComponent {
                        component.step, value(component.value) });
                });

                return (Inst*)cloned;
            }
            case Value::Borrow: {
                auto& borrow = (InstBorrow&)instruction;
                return (Inst*)createInst<InstBorrow>(module, function, into, source, name, type,
                                                     place(borrow.place), borrow.mut);
            }
            case Value::Address:
                return (Inst*)createInst<InstAddress>(module, function, into, source, name, type,
                                                      place(((InstAddress&)instruction).place));
            case Value::Copy: {
                auto& copy = (InstCopy&)instruction;
                auto cloned = createInst<InstCopy>(module, function, into, source, name, type,
                                                   place(copy.place));
                cloned->copy = copy.copy;
                cloned->local = copy.local == maxLimit<U32> ? maxLimit<U32> : clone.locals[copy.local];
                return (Inst*)cloned;
            }
            case Value::Move: {
                // The sink travels unchanged: which function relocates a type is a property of the
                // type, and the type did not move. See clonableKind for why the instruction may.
                auto& move = (InstMove&)instruction;
                auto cloned = createInst<InstMove>(module, function, into, source, name, type,
                                                   place(move.place));
                cloned->sink = move.sink;
                return (Inst*)cloned;
            }
            case Value::Drop: {
                /*
                 * A teardown, copied whole - see clonableKind.
                 *
                 * Both halves and both kinds travel unchanged, for the same reason `InstMove::sink`
                 * does: which function tears a type down is a property of the type, and the type did
                 * not move. `releaseStorage` travels for the reason `InstAlloc::storage` does - it is
                 * the other half of one statement about one allocation, and splitting them is how a
                 * frame-placed run gets handed to `freeHeap`.
                 *
                 * The place is the only thing rebuilt, and `movesLocal` is what keeps that safe: a
                 * drop re-rooted at the caller's storage would run the caller's teardown here and
                 * again where the caller's own drop sits.
                 */
                auto& drop = (InstDrop&)instruction;
                auto cloned = createInst<InstDrop>(module, function, into, source, name, type,
                                                   place(drop.place), drop.dropKind, drop.reclaimKind);
                cloned->drop = drop.drop;
                cloned->reclaim = drop.reclaim;
                cloned->releaseStorage = drop.releaseStorage;
                return (Inst*)cloned;
            }
            case Value::Swap: {
                // Two places and a content type. The type is a global handle, so it means the same
                // thing in either function, and the sink travels as `Move`'s does.
                auto& swap = (InstSwap&)instruction;
                auto cloned = createInst<InstSwap>(module, function, into, source, name, type,
                                                   place(swap.a), place(swap.b), swap.content);
                cloned->sink = swap.sink;
                return (Inst*)cloned;
            }
            case Value::Exchange: {
                // A write over a value taken out, so it has both a place and an operand - and a
                // result slot, which is a *local* and therefore renumbered like `Copy`'s.
                auto& exchange = (InstExchange&)instruction;
                auto cloned = createInst<InstExchange>(module, function, into, source, name, type,
                                                       place(exchange.place), value(exchange.value));
                cloned->sink = exchange.sink;
                cloned->local = exchange.local == maxLimit<U32> ? maxLimit<U32>
                                                                : clone.locals[exchange.local];
                return (Inst*)cloned;
            }
            case Value::TypeMetric: {
                auto& metric = (InstTypeMetric&)instruction;
                return (Inst*)createInst<InstTypeMetric>(module, function, into, source, name, type,
                                                         metric.of, metric.metric);
            }
            case Value::Symbol: {
                auto& symbol = (InstSymbol&)instruction;
                return (Inst*)createInst<InstSymbol>(module, function, into, source, name, type,
                                                     symbol.callee, symbol.global);
            }
            case Value::Cast: case Value::Bitcast: case Value::Neg: case Value::Not:
            case Value::Sqrt: case Value::Abs: {
                auto& unary = (InstUnary&)instruction;
                return (Inst*)createInst<InstUnary>(module, function, into, source, name, type,
                                                    instruction.kind, value(unary.from));
            }
            // Three operands, so not the arm above: reading it as a Unary would clone the first
            // operand and drop the other two, which is a use nobody records and two values nothing
            // reads. The one kind in this switch with an arity of its own.
            case Value::Fma: {
                auto& fma = (InstFma&)instruction;
                return (Inst*)createInst<InstFma>(module, function, into, source, name, type,
                                                  value(fma.a), value(fma.b), value(fma.c));
            }
            case Value::Select: {
                auto& select = (InstSelect&)instruction;
                return (Inst*)createInst<InstSelect>(module, function, into, source, name, type,
                                                     value(select.cond), value(select.whenTrue),
                                                     value(select.whenFalse));
            }
            case Value::Cmp: {
                auto& compare = (InstCmp&)instruction;
                return (Inst*)createInst<InstCmp>(module, function, into, source, name, type,
                                                  value(compare.lhs), value(compare.rhs), compare.cmp);
            }
            // The vector kinds, copied into the caller. Admitted to `clonableKind` above, so a body
            // reaching here can hold one and this is what builds it.
            case Value::VecSplat: {
                auto& splat = (InstVecSplat&)instruction;
                return (Inst*)createInst<InstVecSplat>(module, function, into, source, name, type,
                                                       value(splat.from));
            }
            case Value::VecLane: {
                auto& lane = (InstVecLane&)instruction;
                return (Inst*)createInst<InstVecLane>(module, function, into, source, name, type,
                                                      value(lane.from), lane.lane);
            }
            case Value::VecWithLane: {
                auto& lane = (InstVecLane&)instruction;
                return (Inst*)createInst<InstVecLane>(module, function, into, source, name, type,
                                                      value(lane.from), lane.lane, value(lane.value));
            }
            case Value::VecShuffle: {
                auto& shuffle = (InstVecShuffle&)instruction;
                auto cloned = createInst<InstVecShuffle>(module, function, into, source, name, type,
                                                         value(shuffle.left), value(shuffle.right));

                for(auto entry: shuffle.pattern) cloned->pattern.push(entry);
                return (Inst*)cloned;
            }
            case Value::VecReduce: {
                auto& reduce = (InstVecReduce&)instruction;
                return (Inst*)createInst<InstVecReduce>(module, function, into, source, name, type,
                                                        value(reduce.from), reduce.reduce);
            }
            case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
            case Value::Shl: case Value::Shr: case Value::Sar:
            case Value::And: case Value::Or: case Value::Xor: {
                auto& binary = (InstBinary&)instruction;
                return (Inst*)createInst<InstBinary>(module, function, into, source, name, type,
                                                     instruction.kind, value(binary.lhs), value(binary.rhs));
            }
            case Value::Call: {
                auto& inner = (InstCall&)instruction;
                auto cloned = createInst<InstCall>(module, function, into, source, name, type,
                                                   inner.callee);

                for(auto argument: inner.args.contents(opt.local)) {
                    cloned->args.push(opt.program.arena, value(argument));
                }

                cloned->local = inner.local == maxLimit<U32> ? maxLimit<U32> : clone.locals[inner.local];
                return (Inst*)cloned;
            }
            case Value::Native: {
                /*
                 * A host node, which owns nothing: `op` and `method` are what to emit and the
                 * argument list is uses, so there is no state here that means anything about the
                 * function it sits in. It has no result local either - a host value is a value.
                 */
                auto& native = (InstNative&)instruction;
                auto cloned = createInst<InstNative>(module, function, into, source, name, type,
                                                     native.op, native.method);

                for(auto argument: native.args.contents(opt.local)) {
                    cloned->args.push(opt.program.arena, value(argument));
                }

                return (Inst*)cloned;
            }
            case Value::CallDyn: {
                // The same as a direct call plus the two operands that stand in for the callee, and
                // a signature - a global type, which is the same handle in either function.
                auto& dynamic = (InstCallDyn&)instruction;
                auto cloned = createInst<InstCallDyn>(module, function, into, source, name, type,
                                                      value(dynamic.callable), value(dynamic.address),
                                                      dynamic.signature);

                for(auto argument: dynamic.args.contents(opt.local)) {
                    cloned->args.push(opt.program.arena, value(argument));
                }

                cloned->handover = dynamic.handover;
                cloned->local = dynamic.local == maxLimit<U32> ? maxLimit<U32>
                                                              : clone.locals[dynamic.local];
                return (Inst*)cloned;
            }
            default:
                // `describe` refused every other kind before this call site was ever considered.
                return nullptr;
        }
    }

    /*
     * A terminator, against the caller - which for a `ret` is the whole of what the graft is.
     *
     * A `ret` becomes a jump to the block holding whatever followed the call. That is the same
     * statement the straight-line case makes by not emitting anything at all: control arrives at
     * what came after the call, and the value the caller was going to read is whatever the `ret`
     * named. Here it is an edge rather than an absence, and the phi at the other end is what makes
     * several of them one value.
     */
    Inst* cloneTerminator(Clone& clone, Candidate& candidate, Block& into,
                          Value& terminator, ModulePtr<Block> continuation) {
        auto& module = *opt.module;
        auto& function = *opt.function;
        auto source = terminator.source;
        auto type = terminator.type;

        switch(terminator.kind) {
            case Value::Ret:
                return (Inst*)createInst<InstJmp>(module, function, into, source, StringId(), type,
                                                  continuation);

            // The one terminator that stays itself: a `ret` becomes the jump back to what follows
            // the call, and this is the block that has nothing to go back to.
            case Value::Unreachable:
                return (Inst*)createInst<InstUnreachable>(module, function, into, source, StringId(),
                                                          type);

            case Value::Jmp:
                return (Inst*)createInst<InstJmp>(module, function, into, source, StringId(), type,
                                                  mapBlock(clone, ((InstJmp&)terminator).target));
            case Value::Je: {
                auto& branch = (InstJe&)terminator;
                return (Inst*)createInst<InstJe>(module, function, into, source, StringId(), type,
                                                 mapValue(clone, branch.cond),
                                                 mapBlock(clone, branch.thenBlock),
                                                 mapBlock(clone, branch.elseBlock));
            }
            default:
                return nullptr;
        }
    }

    /*
     * What gives each new local its storage, before anything rooted in one is added to a block.
     *
     * From the callee's own slot rather than from the instruction kind, which is what makes this
     * complete: an `Alloc` is the common case, but a `Copy` of an aggregate and a `Call` returning
     * one each own a local too, and a slot left holding null is storage that later looks to every
     * pass like a local nothing allocated. Order matters as well - `addPlaceUse` reads the slot to
     * record a use, and it runs when an instruction reaches its block.
     */
    void bindLocals(Clone& clone, Candidate& candidate) {
        for(U32 local = 0; local < candidate.callee->localCount(); local++) {
            auto index = clone.locals[local];
            if(index == maxLimit<U32>) continue;

            auto source = candidate.callee->localAt(opt.local, local).value;
            if(!source) continue;

            auto mapped = clone.values.getValue(U32(source));
            if(!mapped) continue;

            opt.ir().setLocalValue(index, ModulePtr<Value>(mapped.unwrap()));
        }
    }

    /*
     * The straight-line splice: one block's worth of instructions, in front of the call.
     *
     * Answers the value the call becomes, or null where the callee returned nothing. Nothing about
     * the caller's control flow changes, which is the whole reason this case is kept apart from the
     * one below rather than expressed as an instance of it.
     */
    Maybe<ModulePtr<Value>> spliceStraightLine(Clone& clone, Candidate& candidate, Block& block,
                                               Size index) {
        auto body = opt.local[candidate.blocks[0]];

        for(auto instructionPointer: body->instructions(opt.local)) {
            auto& instruction = *opt.local[instructionPointer];
            auto cloned = cloneInstruction(clone, candidate, block, instruction);
            if(!cloned) return Nothing();

            *clone.values.add(U32(instructionPointer)).value = U32(cloned - opt.local);
            clone.emitted.push(cloned);
        }

        bindLocals(clone, candidate);
        opt.ir().insert(block, index, clone.emitted);

        auto& ret = (InstRet&)*opt.local[body->terminator()];
        return Just(ret.value ? mapValue(clone, ret.value) : nullptr);
    }

    /*
     * The graft: the callee's blocks, cloned into the gap the split left.
     *
     * The order below is the part that is not obvious, and every step of it is a thing that has to
     * exist before the step after it can name it:
     *
     *  1. **the blocks**, so that a branch has a target and the caller's own block is the entry;
     *  2. **the phis**, empty, so that a value defined further round a loop is already something the
     *     map can answer with - the one case reverse postorder does not cover on its own;
     *  3. **the instructions and terminators**, in that order, which is where every operand is
     *     translated;
     *  4. **the phi inputs**, once every value they could name exists;
     *  5. **the locals**, which have to hold their storage before an instruction rooted in one is
     *     added to a block, since that is when the use is recorded;
     *  6. **the block contents**, added at last - `IrEditor::append` is what records a use and an
     *     edge, so nothing before this point is visible to a walk of the IR.
     *
     * Answers the value the call becomes: the one `ret`'s value where there is one such block, and
     * otherwise a phi in the continuation over all of them.
     */
    Maybe<ModulePtr<Value>> spliceControlFlow(Clone& clone, Candidate& candidate, Inst& site,
                                              Block& block, Size index) {
        auto& module = *opt.module;
        auto& function = *opt.function;

        auto continuation = opt.ir().splitBlock(block, index);
        auto continuationPointer = (ModulePtr<Block>)(continuation - opt.local);

        /*
         * The storage a copied result is written into - see `describe`.
         *
         * In the caller's own block and ahead of everything the graft emits, which is the whole of
         * what it needs: that block dominates every cloned one, so each returning path has the slot
         * to write into and the continuation has it to read out of.
         */
        InstAlloc* resultStorage = nullptr;
        if(candidate.copiedResult) {
            resultStorage = createInst<InstAlloc>(module, function, block, site.source, site.name,
                                                  site.type, candidate.callerResultSlot);
            opt.ir().append(block, resultStorage);
        }

        // Inline: the blocks of one callee, at one call site. Nothing points into it - the entries
        // hold the two block pointers rather than being pointed at - so growth past 16 is safe.
        SmallArray<ClonedBlock, 16> cloned;
        for(Size i = 0; i < candidate.blocks.size(); i++) {
            ClonedBlock entry;
            entry.from = candidate.blocks[i];

            // The caller's own block is the callee's entry, which is what keeps the straight-line
            // prefix of a branching callee in the block the passes after this one can see it in.
            entry.to = i == 0 ? &block
                              : function.addBlock(module, opt.local[candidate.blocks[i]]->name);
            entry.to->source = opt.local[candidate.blocks[i]]->source;

            *clone.blocks.add(U32(candidate.blocks[i])).value = U32((ModulePtr<Block>)(entry.to - opt.local));
            cloned.push(::move(entry));
        }

        for(auto& target: cloned) {
            for(auto phiPointer: opt.local[target.from]->phis(opt.local)) {
                auto& phi = *opt.local[phiPointer];
                auto copy = createInst<InstPhi>(module, function, *target.to, phi.source, phi.name,
                                                phi.type);

                *clone.values.add(U32(phiPointer)).value = U32((ModulePtr<Value>)(copy - opt.local));
                target.phis.push((Inst*)copy);
            }
        }

        for(auto& target: cloned) {
            auto from = opt.local[target.from];

            for(auto instructionPointer: from->instructions(opt.local)) {
                auto& instruction = *opt.local[instructionPointer];
                auto copy = cloneInstruction(clone, candidate, *target.to, instruction);

                /*
                 * Asserted rather than declined, which the straight-line case can afford not to be.
                 *
                 * By this point the caller's block has already been cut in two, so there is no
                 * "leave it alone" left to return to - backing out would mean putting the halves
                 * back together. It cannot happen either: `describe` checked every instruction
                 * against `clonableKind` and every terminator against `clonableTerminator` before
                 * the call site was considered, so a null here is a kind admitted by one of those
                 * lists and missing from the switch that copies it.
                 */
                assertTrue(copy != nullptr);

                *clone.values.add(U32(instructionPointer)).value = U32(copy - opt.local);
                target.instructions.push(copy);
            }

            target.terminator = cloneTerminator(clone, candidate, *target.to,
                                                *opt.local[from->terminator()], continuationPointer);
            assertTrue(target.terminator != nullptr);
        }

        for(auto& target: cloned) {
            auto from = opt.local[target.from];
            Size i = 0;

            for(auto phiPointer: from->phis(opt.local)) {
                auto& phi = *opt.local[phiPointer];
                auto copy = (InstPhi*)target.phis[i++];

                for(auto input: phi.inputs.contents(opt.local)) {
                    // An alternative arriving from a block nothing reaches is dropped with the block
                    // - there is no edge for it to arrive over, so the copy has one fewer input and
                    // one fewer predecessor to match it.
                    auto source = mapBlock(clone, input.block);
                    if(!source) continue;

                    copy->inputs.push(opt.program.arena, PhiInput {
                        source, mapValue(clone, input.value)
                    });
                }
            }
        }

        /*
         * And each returning path's copy into it, which is the `ret` itself: lowering emits exactly
         * this write into the return place the caller passed, so what the graft does is name the
         * caller's slot in place of that hidden one.
         */
        if(candidate.copiedResult) {
            for(auto& target: cloned) {
                auto& terminator = *opt.local[opt.local[target.from]->terminator()];
                if(terminator.kind != Value::Ret) continue;

                auto returned = ((InstRet&)terminator).value;
                if(!returned) continue;

                target.instructions.push((Inst*)createInst<InstInit>(
                    module, function, *target.to, terminator.source, StringId(),
                    module.scalar.unit, Place::inLocal(candidate.callerResultSlot),
                    mapValue(clone, returned), Value::Init));
            }
        }

        bindLocals(clone, candidate);

        for(auto& target: cloned) {
            for(auto phi: target.phis) opt.ir().append(*target.to, phi);
            for(auto instruction: target.instructions) opt.ir().append(*target.to, instruction);
            opt.ir().append(*target.to, target.terminator);
        }

        /*
         * And what the call becomes.
         *
         * One returning block dominates the continuation on its own, so its value is simply the
         * answer. Several do not - that is the definition of a join - and the phi is what says so.
         * It is built after the terminators rather than with them because its inputs are the blocks
         * those terminators created the edges from.
         *
         * A copied result is neither: every path wrote into the storage above, so there is one
         * value already and no join to build.
         */
        if(resultStorage) return Just((ModulePtr<Value>)((Value*)resultStorage - opt.local));

        Array<PhiInput> results;
        for(auto blockPointer: candidate.returns) {
            auto& ret = (InstRet&)*opt.local[opt.local[blockPointer]->terminator()];
            if(!ret.value) continue;

            results.push(PhiInput { mapBlock(clone, blockPointer),
                                    mapValue(clone, ret.value) });
        }

        if(results.isEmpty()) return Just(ModulePtr<Value>(nullptr));
        if(results.size() == 1) return Just(results[0].value);

        // Typed from the call rather than from one of the values it merges: the call's type is the
        // callee's declared result, which is the one thing every `ret` in it already agreed on.
        auto phi = createInst<InstPhi>(module, function, *continuation, site.source, site.name,
                                       site.type);
        for(auto& input: results) phi->inputs.push(opt.program.arena, input);

        opt.ir().append(*continuation, phi);
        return Just((ModulePtr<Value>)((Value*)phi - opt.local));
    }

    /*
     * One call, replaced.
     *
     * The caller's local table grows by the callee's locals, minus the `&` parameters' - those name
     * the caller's own storage and are rewritten to borrow roots instead of being given slots here.
     * A memory-typed result reuses the slot the call already had rather than adding one, which is
     * what keeps the caller's existing places rooted in it pointing at the same storage.
     */
    bool inlineCall(Block& block, Size index, ModulePtr<Inst> pointer, bool& grafted) {
        auto& call = (InstCall&)*opt.local[pointer];
        if(!call.callee) return false;
        if(call.callee == (ModulePtr<Function>)(opt.function - opt.local)) return false;

        site = pointer;
        auto described = describe(call.callee);
        if(!described) return false;

        auto candidate = described.unwrap();
        if(candidate.callee->args.size() != call.args.size()) return false;

        for(auto argument: call.args.contents(opt.local)) candidate.arguments.push(argument);

        return inlineSite(candidate, block, index, pointer, grafted);
    }

    /*
     * A site whose callee is settled and whose arguments are collected, spliced.
     *
     * Written against `candidate.arguments` rather than against any instruction's own list, which is
     * what lets the two call forms share it: a `calldyn` this pass resolved passes the environment
     * word in front of the declared arguments, and a direct call passes its list unchanged.
     */
    /*
     * A collapse, recorded once the copy exists - see `collapsesCycle`, which is where both halves
     * of this are read. Called from every commit rather than from the description, because a
     * description is speculative and this is the point there is something to record.
     *
     * A recursive candidate is one only `collapsesCycle` admits, so reaching here with one *is* the
     * collapse: the caller has spent its one member, and every call the copy brought with it is a
     * site the next level would take.
     */
    void recordCollapse(Candidate& candidate) {
        if(!recursive.get(U32(candidate.pointer))) return;

        absorbed.add(U32(currentFunction()), U32(candidate.pointer));

        for(auto entry: cloneScratch.values.entries()) {
            if(performsCall(opt.local[ModulePtr<Value>(entry.value)]->kind)) {
                collapsed.add(entry.value, true);
            }
        }
    }

    bool inlineSite(Candidate& candidate, Block& block, Size index, ModulePtr<Inst> pointer,
                    bool& grafted) {
        auto& call = *opt.local[pointer];

        /*
         * The `&` arguments, which have to be borrows of storage that exists rather than of storage
         * a target stood in for.
         *
         * Design.md's tier 1: a mutable borrow of a *packed* field has no address to hand over, so
         * a target materializes the field into a temporary, passes that, and writes it back when the
         * loan ends - and the point the loan ends is the call. Splicing the call away takes the
         * write-back with it, which is a value silently not stored. `Local::materialized` and
         * resolve/lower.cpp are that write-back; the JS target had a second one and no longer does,
         * since a reference there names the slot rather than holding a copy of it - see refIsTriple.
         *
         * A borrow of a *whole local* is never that. There is nothing above it to be packed into, so
         * both targets hand over the storage itself - which is also the case worth inlining, since
         * the box a managed target keeps such a local in is exactly the allocation that stops
         * existing once the callee's reads and writes are the caller's own.
         *
         * Anything that is not an `InstBorrow` is a reference the program already had - a `&`
         * parameter passed straight on, a borrow returned by something - and nothing was
         * materialized for it.
         */
        for(Size i = 0; i < candidate.parameters.size(); i++) {
            if(candidate.parameters[i].binding != Binding::Borrowed) continue;

            auto argument = candidate.arguments[i];
            if(!argument || opt.local[argument]->kind != Value::Borrow) continue;

            auto& borrow = (InstBorrow&)*opt.local[argument];
            if(borrow.place.root != PlaceRoot::Local) return false;
            if(borrow.place.projections.isNotEmpty()) return false;
        }

        /*
         * And the memory-typed arguments, which have to be storage this frame can name.
         *
         * `storageOf` is the same question the flattening pass asks of the same values, and it
         * answers for the two shapes the resolver produces: a load of a place, and a value some
         * local's storage came from. Anything else - a memory-typed value with no place behind it -
         * is one there is nothing to re-root at, so the call stays a call.
         */
        for(Size i = 0; i < candidate.parameters.size(); i++) {
            if(candidate.parameters[i].binding != Binding::Memory) continue;

            auto storage = storageOf(opt, candidate.arguments[i]);
            if(!storage) return false;

            candidate.parameters[i].storage = storage.unwrap();
        }

        /*
         * A call that produces nothing and that something still reads as a value.
         *
         * The graft has no result to hand back for one - a unit value has no representation, so the
         * splice produces no value at all - and the site is removed from its block afterwards. That
         * is fine for the readers the removal *does* settle, which are the slots that named the call
         * as their storage; it is not fine for one that names it as an *operand*, because there is
         * nothing to point that operand at and no constant for `{}` to invent.
         *
         * `let chosen = if c then unitValue() else identity(unitValue())` is the case: the join is a
         * unit-typed phi whose alternatives are the two calls, and inlining one of them left the phi
         * naming an instruction that was no longer in any block - and the *next* graft then wrote
         * that dead value into `chosen`'s slot as its own result. Invisible in every dump, because
         * nothing below resolve reads a unit value; found by verifyFunction.
         *
         * Declined rather than repaired, and it costs nothing worth having: what is not inlined is a
         * call whose whole result is a value with no bytes.
         */
        if(isUnit(opt.global, call.type)) {
            auto read = false;

            for(auto user: call.uses(opt.local)) {
                eachOperand(opt.local, *opt.local[user], [&](ModulePtr<Value> operand) {
                    if(operand == (ModulePtr<Value>)pointer) read = true;
                });
            }

            if(read) return false;
        }

        if(!worthInlining(candidate)) return false;
        if(!graft(candidate, block, index, pointer, grafted)) return false;

        recordCollapse(candidate);

        // `removeInstruction` rather than `eraseInstruction`, which asserts that nothing reads the
        // instruction - true of a call whose result the graft replaced and not of one returning
        // unit, whose place-root uses are recorded on the locals it named. The operand case is
        // declined above, which is what makes those the only ones left.
        opt.ir().removeInstruction(pointer);

        opt.changed = true;
        return true;
    }

    /*
     * Which function a `calldyn` reaches, where the value it is reached through was built here.
     *
     * A function value is `{code, env}` and nothing else, so this is one question about its storage:
     * which symbol filled the code word, and what filled the environment word beside it. Both have
     * to be answered together - the body being copied reads its captures out of the environment,
     * and a code word without the environment that goes with it names a body with nothing to read.
     *
     * The walk is over the *uses of the storage*, which is what makes it a reaching-definition
     * answer rather than a guess: a place rooted in a local is recorded as a use of that local's
     * value, so anything that could write either word is in this list. One write of each word and
     * nothing this does not recognize is the whole of what it takes; a second write of the code word
     * - `let f = if c then A else B` - is exactly the merge that makes the callee a run-time fact,
     * and it answers nothing here.
     *
     * The same shape `HeaderReach::contained` in opt_closure.cpp walks, asked for the callee rather
     * than for the teardown. It is not shared with it because the two want opposite things from the
     * `CallDyn` case: that pass needs the value never to be *called with* rather than called, and
     * this one is standing at the call.
     */
    struct DynamicCallee {
        ModulePtr<Function> callee = nullptr;
        ModulePtr<Value> env = nullptr;
    };

    /*
     * Which local's storage a function value is, in the two spellings a call site has of it.
     *
     * The value the site names is the allocation itself where the closure was built in this block,
     * and a read of the whole local where it was built further up and a `load` fetched it back -
     * which is what an inlined `load [%env].f` collapses to once the borrow it went through has
     * been forwarded. Both name one slot, and the slot is what the walk below is about.
     */
    Maybe<U32> functionValueLocal(ModulePtr<Value> callable) {
        auto value = opt.local[callable];

        if(value->kind == Value::Alloc) return Just(((InstAlloc*)value)->local);

        if(value->kind == Value::LoadPlace) {
            auto& place = ((InstLoadPlace*)value)->place;
            if(place.root != PlaceRoot::Local) return Nothing();
            if(const_cast<Place&>(place).projections.isNotEmpty()) return Nothing();

            return Just(place.local);
        }

        return Nothing();
    }

    /*
     * `strict` is what tells the rewrite from the estimate.
     *
     * `inlineDynamicCall` is about to *replace* a call, so nothing it cannot account for may name
     * the storage: a use this does not recognize is a way the two words could have been changed
     * between the write it found and the call it is standing at.
     *
     * `worthInlining` is asking a budget question about a call site that hands the closure to the
     * callee being weighed, and that hand-over is itself a use no strict walk admits - so the strict
     * form answers "no" for every site the bonus exists for. What the relaxed form drops is only
     * that: the writes are still the whole of what decides the answer, and the use that made it
     * relaxed is the one the graft removes.
     */
    Maybe<DynamicCallee> knownCallee(ModulePtr<Value> callable, bool strict = true) {
        if(!callable) return Nothing();

        auto found = functionValueLocal(callable);
        if(!found) return Nothing();

        auto local = found.unwrap();
        if(local >= opt.function->localCount()) return Nothing();

        auto storageValue = opt.function->localAt(opt.local, local).value;
        if(!storageValue || opt.local[storageValue]->kind != Value::Alloc) return Nothing();

        auto storage = opt.local[storageValue];

        ModulePtr<Value> code = nullptr;
        ModulePtr<Value> env = nullptr;

        for(auto user: storage->uses(opt.local)) {
            auto& instruction = *opt.local[user];

            /*
             * Which of the two words a place names, or nothing.
             *
             * **A leading `Downcast` is part of the shape and is skipped**, which is what this was
             * missing. A function value is a record, and a place stepping into a record with one
             * constructor is written `[Downcast 0][Field n]` rather than `[Field n]` - so the writes
             * every closure the resolver builds performs were read here as "a place I do not
             * recognize", and `knownCallee` answered Nothing for all of them. The consequence was
             * not a missed optimization but a missing one: `inlineDynamicCall` is what flattens an
             * adaptor chain and it was reachable only for the closures written the other way.
             *
             * Skipping it is exact rather than approximate. A downcast selects a constructor, a
             * function value has exactly one, and the field indices below it are the layout's own -
             * so the step names the same storage it started from. See §34.6 of
             * test/bench/findings.md.
             */
            auto word = [&](const Place& place) -> Maybe<U32> {
                if(place.root != PlaceRoot::Local || place.local != local) return Nothing();

                auto& projections = const_cast<Place&>(place).projections;
                Size at = 0;

                if(projections.size() == 2 &&
                   projections.get(opt.local, 0).kind == ProjectionKind::Downcast) {
                    at = 1;
                } else if(projections.size() != 1) {
                    return Nothing();
                }

                auto projection = projections.get(opt.local, at);
                if(projection.kind != ProjectionKind::Field) return Nothing();
                if(projection.index != FunValueLayout::kCode &&
                   projection.index != FunValueLayout::kEnv) return Nothing();

                return Just(U32(projection.index));
            };

            switch(instruction.kind) {
                case Value::Init: case Value::Assign: {
                    auto& write = (InstInit&)instruction;
                    auto index = word(write.place);
                    if(!index) return Nothing();

                    // One write of each word. A second is a merge, which is the case with no
                    // compile-time answer at all.
                    auto& slot = index.unwrap() == FunValueLayout::kCode ? code : env;
                    if(slot) return Nothing();

                    slot = write.value;
                    break;
                }

                /*
                 * Reading a word where it lies, which moves nothing - and reading the *whole* value,
                 * which is the shape a resolved capture leaves behind: `load %local` of a function
                 * value stands for the storage rather than for a copy of it, so it is admitted only
                 * where every reader of it is a call through it.
                 */
                case Value::LoadPlace: {
                    if(!strict) break;

                    auto& read = (InstLoadPlace&)instruction;
                    if(word(read.place)) break;

                    if(read.place.root != PlaceRoot::Local || read.place.local != local) {
                        return Nothing();
                    }

                    if(const_cast<Place&>(read.place).projections.isNotEmpty()) return Nothing();

                    auto readPointer = (ModulePtr<Value>)(&read - opt.local);
                    for(auto reader: read.uses(opt.local)) {
                        auto& through = *opt.local[reader];
                        if(through.kind != Value::CallDyn) return Nothing();
                        if(((InstCallDyn&)through).callable != readPointer) return Nothing();
                    }

                    break;
                }

                /*
                 * Calling it, and lending it. Neither can change either word: a call reads them, and
                 * a shared borrow is the one thing the borrow checker guarantees nothing writes
                 * through. Both are how a closure reaches the loop that runs it.
                 *
                 * A `calldyn` naming the value as an *argument* rather than as its callee is a
                 * different statement and is not admitted - it hands the closure to a body this does
                 * not have, which is the same reason `Address` and a mutable borrow are refused by
                 * falling through.
                 */
                case Value::CallDyn:
                    if(strict && ((InstCallDyn&)instruction).callable != callable) return Nothing();
                    break;

                case Value::Borrow:
                    if(strict && ((InstBorrow&)instruction).mut) return Nothing();
                    break;

                default:
                    if(strict) return Nothing();
                    break;
            }
        }

        if(!code || !env) return Nothing();
        if(opt.local[code]->kind != Value::Symbol) return Nothing();

        auto callee = ((InstSymbol*)opt.local[code])->callee;
        if(!callee) return Nothing();

        return Just(DynamicCallee { callee, env });
    }

    /*
     * One call through a function value this frame built, replaced by the body it reaches.
     *
     * This is the transformation an adaptor chain needs and the only one that flattens it. `for x in
     * mapped(xs, f)` is a loop in one function calling a continuation in another through a value,
     * and every link of the chain is one of these: until the call is resolved, the loop body is an
     * indirect call the passes below cannot see into, and once it is spliced the captures it reads
     * are the caller's own locals - which is what the next round then folds.
     *
     * There is no direct-call form in between. The code word names a `takesEnv` body, whose
     * convention only the value form spells, so what this leaves behind is the body itself and never
     * a call to it - see `describe`.
     */
    bool inlineDynamicCall(Block& block, Size index, ModulePtr<Inst> pointer, bool& grafted) {
        auto& call = (InstCallDyn&)*opt.local[pointer];
        if(!call.callable) return false;

        auto resolved = knownCallee(call.callable);
        if(!resolved) return false;

        auto target = resolved.unwrap();
        if(target.callee == (ModulePtr<Function>)(opt.function - opt.local)) return false;

        site = pointer;
        auto described = describe(target.callee, false, true);
        if(!described) return false;

        auto candidate = described.unwrap();
        candidate.dynamic = true;

        // The environment in front of the declared arguments, which is what `takesEnv` says the
        // callee's parameter list is.
        if(candidate.callee->args.size() != call.args.size() + 1) return false;

        /*
         * And the storage the environment word names, which the body's every read of a capture is a
         * place in. Without it the copy is `%()` with a field step on it - see `Binding::Pointer`.
         *
         * The word is `addressof %local` for every closure this pass can resolve at all, because
         * that is what `knownCallee` walked to find the code word beside it. Anything else is an
         * environment whose storage this frame cannot name, and the call stays a call.
         */
        auto environment = opt.local[target.env];
        if(environment->kind != Value::Address) return false;

        candidate.parameters[0].binding = Binding::Pointer;
        candidate.parameters[0].storage = ((InstAddress*)environment)->place;

        candidate.arguments.push(target.env);
        for(auto argument: call.args.contents(opt.local)) candidate.arguments.push(argument);

        return inlineSite(candidate, block, index, pointer, grafted);
    }

    /*
     * One `drop`, replaced by what it would have run.
     *
     * This is Implementation-Containers.md §13.2's third step and it is here rather than anywhere
     * else because a teardown is not reached by a `Call`: `drop %xs reclaim R` names `R` in the
     * instruction, and lowering is what turns it into a call. So the inliner never saw it, and the
     * placement switch §2 promises folds - `Inline`/`Stack`/`Region` release nothing, `Heap` calls
     * `freeHeap` - could not reach the frame that placed the run and knows which of those it is.
     *
     * What the graft needs from the site is a place, and a drop has exactly one: `dropped.place` is
     * the storage being torn down, and the teardown's `->` parameter *is* that storage. So it binds
     * as `Binding::Memory` with the drop's own place, and everything else is the ordinary copy.
     *
     * Three things are declined and each is a case with no single answer here:
     *
     *  - **both halves present.** A `Drop` runs its `drop` and then its `reclaim`, in that order,
     *    and splicing two bodies into one position is two grafts whose second has to land after the
     *    first. Worth doing and not needed yet: a container whose elements owe a drop has ownership
     *    instructions in that half, which `clonableKind` refuses anyway;
     *  - **`releaseStorage`.** The instruction has a job of its own beyond the callee's, so it
     *    cannot simply go;
     *  - **an erased teardown**, which `describe` already refuses along with every other generic
     *    body: what runs is whatever the caller's descriptor holds, and there is no callee to copy.
     */
    bool inlineTeardown(Block& block, Size index, ModulePtr<Inst> pointer, bool& grafted) {
        auto& dropped = (InstDrop&)*opt.local[pointer];

        if(dropped.releaseStorage) return false;
        if(dropped.drop && dropped.reclaim) return false;

        auto callee = dropped.drop ? dropped.drop : dropped.reclaim;
        if(!callee) return false;
        if(callee == (ModulePtr<Function>)(opt.function - opt.local)) return false;

        site = pointer;
        auto described = describe(callee, true);
        if(!described) return false;

        auto candidate = described.unwrap();
        if(candidate.callee->args.size() != 1) return false;

        /*
         * The parameter has to be one the body reaches *through*, which is what `Binding::Memory`
         * records. A teardown that never touches its own storage runs nothing at all and would have
         * been elided rather than emitted; if one arrives anyway, there is no argument value for the
         * `Arg` to map to and the honest answer is to leave the drop alone.
         */
        if(candidate.parameters[0].binding != Binding::Memory) return false;
        candidate.parameters[0].storage = dropped.place;

        /*
         * And the budget, which this site used to be exempt from by accident.
         *
         * `dischargeOwnership` ran in front of this pass, so every non-generic `Drop` had already
         * become the calls it stands for and the only ones reaching here were the erased ones -
         * which `describe` refuses anyway. This path was therefore unreachable for the bodies it was
         * written for, and never having asked cost nothing.
         *
         * With the discharge behind the inliner it is live for every drop in the program, and
         * exempting it means copying a teardown into a site whatever it costs: `Matrix`'s inner loop
         * took a twelve-instruction `Reclaim` with a branch in it where it had held a two-instruction
         * call, and measured 0.933x for it. A teardown is a callee like any other - the site is a
         * call by the time anything emits it, which is exactly what the budget is denominated in.
         */
        if(!worthInlining(candidate)) return false;
        if(!graft(candidate, block, index, pointer, grafted)) return false;
        recordCollapse(candidate);

        // The drop's own reads go with it: the storage its place was rooted in, and any index in the
        // path. Both are recorded on the values rather than in an operand slot, which is why this
        // has to be `removeInstruction` rather than a walk of an argument list.
        opt.ir().removeInstruction(pointer);

        opt.changed = true;
        return true;
    }

    /*
     * The graft itself, once the site has said what it passes and where.
     *
     * Everything from here down is about the callee, which is what lets a `drop` reach it at all:
     * the two site kinds differ entirely in how the parameters are bound and in what is tidied up
     * afterwards, and not at all in what a copy of a body is.
     */
    bool graft(Candidate& candidate, Block& block, Size index, ModulePtr<Inst> pointer,
               bool& grafted) {
        auto& site = *opt.local[pointer];

        auto& clone = cloneScratch;
        clone.clear();
        clone.into = &block;
        auto& module = *opt.module;

        /*
         * The caller's slots for this site's result, of which there can be more than one.
         *
         * `InstCall::local` is the slot the call was *given*, and it is not always the slot the body
         * reads through: a class default reached through an instance ends up with two slots naming
         * one call, and `storageOf` - which is what wrote the reads, when opt_arg.cpp took the
         * record apart at the next call - answers with the lowest. So the callee's returned storage
         * is mapped onto the lowest, and every one of them is repointed at the clone afterwards.
         * Getting this wrong is invisible in the resolve IR and shows up as a backend reading a
         * local nothing allocated. A `drop` site has none of this: it names no slot and the
         * teardown it runs answers nothing.
         */
        Array<U32> resultSlots;
        for(U32 local = 0; local < opt.function->localCount(); local++) {
            if(opt.function->localAt(opt.local, local).value != (ModulePtr<Value>)pointer) continue;

            resultSlots.push(local);
        }

        // A slot holds the one value its storage came from, and a phi over several `ret`s is not
        // one - see the memory-result reasoning in `describe`, which is the same argument from the
        // callee's side. `copiedResult` is that case answered rather than declined: the graft makes
        // the storage itself and every returning path writes into it, so there is one value and the
        // slot holds it.
        if(resultSlots.isNotEmpty() && candidate.returns.size() > 1 && !candidate.copiedResult) {
            return false;
        }

        auto resultSlot = resultSlots.size() ? resultSlots[0]
            : site.kind == Value::Call ? ((InstCall&)site).local : maxLimit<U32>;

        /*
         * And the slot a copied result is written into, which is the one thing this shape needs
         * that the site may not already have: a call whose memory-typed answer nothing reads is
         * given no local, and the copy still needs somewhere to land.
         */
        if(candidate.copiedResult) {
            if(resultSlot == maxLimit<U32>) {
                resultSlot = opt.function->addLocal(module, site.type, site.name, nullptr,
                                                    ast::BindType::Borrow);
            }

            candidate.callerResultSlot = resultSlot;
        }

        for(U32 local = 0; local < candidate.callee->localCount(); local++) {
            auto slot = candidate.callee->localAt(opt.local, local);

            if(rerootedParameter(candidate, local)) {
                // Never read: every place rooted in it is rewritten against the caller instead.
                // Given an out-of-range value so that a path missing that rewrite trips rather than
                // silently naming local zero.
                clone.locals.push(maxLimit<U32>);
                continue;
            }

            if(local == candidate.resultLocal && resultSlot != maxLimit<U32>) {
                clone.locals.push(resultSlot);
                continue;
            }

            clone.locals.push(opt.function->addLocal(module, slot.type, slot.name, nullptr,
                                                     slot.convention, false, slot.closureEnv));
        }

        // The callee's arguments, as the values the site passed. A re-rooted parameter's `Arg` is
        // reached through its local rather than as an operand, so this covers the value case and
        // costs nothing in the others - and a `drop` passes nothing, which is an empty list here.
        for(Size i = 0; i < candidate.arguments.size(); i++) {
            auto argPointer = (ModulePtr<Value>)candidate.callee->args.get(opt.local, i);
            *clone.values.add(U32(argPointer)).value = U32(candidate.arguments[i]);
        }

        auto spliced = candidate.isStraightLine()
            ? spliceStraightLine(clone, candidate, block, index)
            : spliceControlFlow(clone, candidate, site, block, index);

        if(!spliced) return false;
        if(!candidate.isStraightLine()) grafted = true;

        auto result = spliced.unwrap();

        if(result && opt.local[pointer]->useCount() != 0) {
            opt.ir().replaceValue((ModulePtr<Value>)pointer, result);
        }

        // And the slots that named the site as their storage, which is not a use and so is not
        // something `replaceValue` reaches - a place rooted in one of them is recorded against the
        // *value* the slot holds, and that value is about to stop existing.
        // Lowest last, so that the back edge `setLocalValue` writes - Value::slot, which is what
        // findPlace and backingLocal answer with - names the same slot the scan above took, since
        // both used to read the first match and only one of the two directions can win.
        for(Size i = resultSlots.size(); i-- > 0;) {
            opt.ir().setLocalValue(resultSlots[i], result);
        }

        // And the slot the graft made for a copied result, where the site had none of its own.
        if(candidate.copiedResult && resultSlots.isEmpty()) {
            opt.ir().setLocalValue(candidate.callerResultSlot, result);
        }

        return true;
    }

    /*
     * The caller's blocks, put back into an order in which every definition precedes the uses of it.
     *
     * A graft appends: the half of the caller's block that followed the call becomes a new block at
     * the end of the list, and the callee's own blocks land behind it - which puts the continuation,
     * and the phi in it that merges the returns, in front of the blocks that define what it merges.
     *
     * That is not a cosmetic ordering. `lowerProgram` walks a function's blocks in list order and
     * asserts that every operand it meets has already been lowered - phis excepted, which is why a
     * loop works there at all - so an out-of-order block list is `resolve value was used before it
     * was lowered` rather than a differently-shaped dump. Reverse postorder is exactly the property
     * it wants: a block precedes everything it dominates, and a non-phi use is dominated by its
     * definition.
     *
     * Whatever the walk does not reach is kept, at the end. Deleting unreachable blocks is
     * opt_branch.cpp's job and it does it with the phi bookkeeping that belongs to it.
     */
    void reorderBlocks() {
        SmallArray<ModulePtr<Block>, 12> order;
        orderBlocks(*opt.function, order);

        for(auto pointer: opt.function->blocks.contents(opt.local)) {
            if(!order.containsValue(pointer)) order.push(pointer);
        }

        writeBlocks(order);
    }

    void writeBlocks(SmallArray<ModulePtr<Block>, 12>& order) {
        opt.ir().setBlockOrder(Buffer<ModulePtr<Block>>(order.pointer(), order.size()));
    }

    /*
     * How many places in the program name each function, recomputed per round since inlining is
     * exactly the thing that changes it.
     *
     * A `drop` counts as a site, because it is one: the teardown it names is a call by the time
     * either backend sees it, and `soleCallSite` would otherwise say a teardown reached from one
     * drop and nothing else has *no* sites - which is the largest bonus in the table handed to a
     * body on the strength of a count that is wrong.
     *
     * **And only sites the program can still reach**, which is `markProgramReachable`'s answer as of
     * this round rather than `resolveProgram`'s - see the call in `inlineCalls`. The stale answer is
     * a superset everywhere else in this stage, where it costs work on a body nothing emits; here it
     * costs a *decision*, because what this count feeds is a threshold. A function this pass has
     * just emptied of callers still holds every call and every `drop` it was written with, and each
     * of those is counted against a callee that no longer has them.
     *
     * `Drop(Node).drop` in test/resolve/OptChain.yana is what that looks like: `drop$Maybe(Node)` is
     * inlined into its one caller, the copy brings the `drop … via Drop(Node).drop` with it, and the
     * original stays behind in a body nothing can reach. Three sites become four, four is exactly
     * `manyCallSites` on a managed target, and a three-instruction teardown is refused by a budget
     * of two that should have been five.
     */
    void countCallSites() {
        callSites.clear();
        codeWords.clear();

        auto record = [&](HashMap<U32, U32>& into, ModulePtr<Function> callee) {
            if(!callee) return;

            auto entry = into.add(U32(callee));
            *entry.value = entry.existed ? *entry.value + 1 : 1;
        };

        for(auto module: opt.program.modules) {
            for(auto pointer: module->functionOrder.contents(opt.local)) {
                if(!opt.local[pointer]->used) continue;

                for(auto blockPointer: opt.local[pointer]->blocks.contents(opt.local)) {
                    for(auto instructionPointer: opt.local[blockPointer]->instructions(opt.local)) {
                        auto& instruction = *opt.local[instructionPointer];

                        if(instruction.kind == Value::Call) {
                            record(callSites, ((InstCall&)instruction).callee);
                        } else if(instruction.kind == Value::Drop) {
                            record(callSites, ((InstDrop&)instruction).drop);
                            record(callSites, ((InstDrop&)instruction).reclaim);
                        } else if(instruction.kind == Value::Symbol) {
                            // The other way a body is reached, and the one a `calldyn` goes through
                            // - see `movesIntoSite`.
                            record(codeWords, ((InstSymbol&)instruction).callee);
                        }
                    }
                }
            }
        }
    }

    bool runFunction(Function& function) {
        opt.function = &function;

        auto inlined = false;
        auto grafted = false;

        /*
         * By index rather than over a snapshot of the block list, because a graft appends to it: the
         * half of the caller's block that followed the call is a new block, and the rest of the
         * caller - including the next call - is in it. Reading the size each step is what lets a
         * chain of branching calls collapse in one walk instead of one per round.
         */
        for(Size b = 0; b < function.blocks.size(); b++) {
            auto block = opt.local[function.blocks.get(opt.local, b)];

            /*
             * Forwards, and re-reading the size each step, because the splice inserts the callee's
             * instructions in front of the call and removes the call itself: the net effect on the
             * index is that the position now holds whatever followed the call, so the walk does not
             * advance on a successful inline. A callee that itself contains a call is therefore
             * considered on this pass too rather than on the next round.
             */
            for(Size i = 0; i < block->instructionCount();) {
                auto pointer = block->instructionAt(opt.local, i);
                auto kind = opt.local[pointer]->kind;

                if(kind != Value::Call && kind != Value::Drop && kind != Value::CallDyn) {
                    i++;
                    continue;
                }

                auto replaced = kind == Value::Call ? inlineCall(*block, i, pointer, grafted)
                              : kind == Value::CallDyn ? inlineDynamicCall(*block, i, pointer, grafted)
                              : inlineTeardown(*block, i, pointer, grafted);

                if(replaced) inlined = true;
                else i++;
            }
        }

        /*
         * And the cleanup the graft owes, only where one happened.
         *
         * A `jmp` chain this pass did not create is one the resolver emitted and both backends
         * already deal with, so there is no reason to normalize a function nothing was spliced into.
         * The merge runs first: it is what decides which blocks there are, and the reordering is
         * about what order the ones that remain are in.
         */
        if(grafted) {
            mergeBlocks(opt);
            reorderBlocks();
        }

        /*
         * And where every allocation lives, re-decided before anything reads the answer.
         *
         * `selectStorage` ran in the ownership stage and inlining is what makes it stale: an
         * allocation is on the heap because the analysis proved it outlives the frame, and the
         * commonest proof is that the function *returns* it. Copying the body into its caller
         * removes the reason, and nothing went back to ask.
         *
         * Not compositional - it re-derives the answer from the collapsed body - so a chain of any
         * depth is one call rather than one rule applied per level. See reselectStorage.
         *
         * **In front of `settle`, and that is the whole of why it is here** rather than once at the
         * end of the stage. The heap answer is a constant the program reads at run time: a run
         * carries a bit saying whether the allocator owns its storage, and the teardown tests it.
         * `settle` is the full optimizer, so it folds that test against whatever the bit says at
         * that moment - and a `releaseRun` folded while the bit still says heap becomes an
         * unconditional `freeHeap` that no later patch of the constant can take back. Demoting
         * first means the fold sees a frame-placed run and removes the teardown entirely, which is
         * what native.cpp's `releaseRun` has claimed all along.
         */
        if(inlined) reselectStorage(*opt.module, function);
        if(inlined) settle(function);
        return inlined;
    }

    /*
     * The folding half of the driver's rounds, run on a function this pass has just changed.
     *
     * **The size a call site is judged against is the callee as it is now**, and until this existed
     * that was the callee as inlining had left it rather than as anything would ever emit it.
     * `grading()` in `Inline.yana` is `graded(5) + graded(50) + graded(500)`: round one splices three
     * copies of a two-branch body into it, which is sixteen blocks, and round two then measured
     * `main`'s one call to it against *that* - past `maxBlocks` before any budget was even consulted.
     * What the emitted function actually is, once the caller's literals have folded through those
     * branches, is `ret 6`. So the call was refused on the strength of a size that existed only
     * between two passes, and `grading()` stayed a call on both targets to a function returning a
     * constant.
     *
     * This is not the optimizer running early. It is the question "what did the last round of
     * inlining actually leave here", which this pass is asking on every call site and was previously
     * answering with a body no backend would ever see.
     *
     * **The driver's own rounds, and not a chosen few of them.** That was the earlier shape and the
     * reasoning behind it - that the place passes, the loop pass and CSE do not change the *size* of
     * what a caller would be copying - stopped being true when the loop pass gained
     * `eliminateDeadLoops`. `Reclaim(Array(Int))` is the case: it is a call containing an O(n) walk
     * until the walk is removed, and it is a comparison afterwards, which is the difference between
     * a callee no site will take and one every site will. Removing the walk needs the place passes
     * in front of it, since what makes the element read dead is a local that scalarization removed -
     * so picking a subset here means picking most of the list and then having to revisit which.
     *
     * Per function and only where something was inlined, so a program whose every call site was
     * refused pays nothing for this. What it costs where something was is one function's worth of
     * rounds per inlining round, against work the driver was going to do on that function anyway.
     */
    void settle(Function& function) {
        opt.function = &function;
        optimizeRounds(opt);
    }

    /*
     * The same statement `settle` makes about a caller, made about a callee before any site is
     * measured against it - and it is the half that was missing.
     *
     * `settle` reaches a body only where something was spliced *into* it, so a leaf is judged in the
     * form the resolver wrote rather than in the form a backend would emit. The two are not close.
     * A mutable local is an `alloc`, a store per assignment and a load per read in that form, and
     * every one of those is gone by the time anything emits it - `mix` in
     * `test/bench/programs/Hash.yana` is twenty-four instructions here and eight afterwards, because
     * five statements over one `let &h` are fifteen instructions of storage traffic that
     * `forwardPlaces`, `promotePlaces` and `scalarizeLocals` remove between them. Judging a body
     * three times its own size is not a conservatism; it is measuring the wrong body.
     *
     * Lazily and once, which is what keeps it from being the driver's loop run twice. The pass walks
     * every function in every module - Core, Native and Collections included, which is around 490
     * bodies a program reaches between one and thirty of - and this reaches only the ones something
     * actually calls, since a callee is settled where a site is being described rather than where
     * the walk arrives. What it costs on top is nothing: the driver optimizes each of those bodies
     * afterwards anyway, and starting from a fixed point is where its own rounds now start.
     *
     * `opt.function` and `opt.module` are the caller's and have to be given back. The callee is a
     * different function by construction - every site refuses a call to the function it is in - so
     * nothing being walked is what this rewrites. That still holds of the one recursive callee
     * `collapsesCycle` admits, which is a member of the caller's cycle and not the caller.
     */
    void settleCallee(ModulePtr<Function> pointer, Function& callee) {
        if(settled.get(U32(pointer))) return;
        settled.add(U32(pointer), true);

        auto function = opt.function;
        auto module = opt.module;

        opt.module = callee.module;
        settle(callee);

        opt.function = function;
        opt.module = module;
    }
};

// A cap on the cascade rather than a termination proof, on the same terms as the driver's own round
// limit: a chain of callees each of which calls the next collapses a level per round. Recursion is
// no longer among the things this bounds - see the header - so what is left for it to bound is a
// call depth deeper than three, which stops being inlined rather than going wrong.
constexpr Size kMaxInlineRounds = 3;

}

void inlineCalls(OptContext& opt) {
    auto policy = policyFor(opt.context.settings.inlining, opt.repr.target.family);

    // `ceiling` is zero only at `InlineLevel::None`, where nothing qualifies and the walk below
    // would be a whole-program traversal that decided nothing.
    if(policy.ceiling == 0) return;

    Inliner inliner { opt, policy };
    addressTaken(opt, inliner.taken);
    findRecursion(opt, inliner.recursive, inliner.cycle);

    for(Size round = 0; round < kMaxInlineRounds; round++) {
        /*
         * Which functions the program can still reach, asked again before anything is counted
         * against them - see `countCallSites`, which is the one reader that needs the fresh answer
         * rather than the safe one.
         *
         * Once per round rather than once per stage because a round is what makes it stale: the
         * previous one's grafts are exactly the references that went away, and this pass runs twice
         * over the program - so the second call's first round starts from an answer the first call
         * spent three rounds invalidating.
         *
         * Cheaper than it reads. It walks what the program reaches while the two loops below walk
         * every function there is, and the `used` test it writes is what then keeps both of them off
         * the bodies it just found nothing can run.
         */
        markProgramReachable(opt.program);
        inliner.countCallSites();

        auto inlined = false;

        for(auto module: opt.program.modules) {
            opt.module = module;

            for(auto pointer: module->functionOrder.contents(opt.local)) {
                auto function = opt.local[pointer];
                if(function->signature || function->blocks.isEmpty()) continue;

                // A body nothing can reach is not one to copy anything into: it is not emitted, and
                // the call sites inside it have just stopped being counted - see countCallSites.
                if(!function->used) continue;

                inlined = inliner.runFunction(*function) || inlined;
            }
        }

        if(!inlined) break;
    }
}
