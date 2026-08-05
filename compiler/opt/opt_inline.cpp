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
 * A **recursive** callee is declined outright, and that is a rule the straight-line half never had
 * to state: a body that calls itself almost always branches on something first, so refusing branches
 * refused recursion as a side effect. It does not any more. Copying a recursive body into a caller
 * copies the recursive call with it, and the copy is *unrolling* rather than inlining - a different
 * transformation, with a cost model this table does not have and a growth rate the round budget
 * bounds only by accident. So the call graph's cycles are found once and every function in one is
 * refused, which covers a self-call and a mutual pair with the same rule. What remains bounded by
 * the round budget is only the honest case: a chain of callees each of which calls the next.
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
            policy.repeatedPenalty = managed ? 3 : 1;
            policy.manyCallSites = managed ? 4 : 8;
            policy.manyPenalty = managed ? 6 : 2;
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
};

struct Parameter {
    Binding binding = Binding::Value;

    // The callee local the two re-rooted cases rewrite away, and `kNone` for the value case.
    U32 local = maxLimit<U32>;

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

    U32 size = 0;

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
void findRecursion(OptContext& opt, HashMap<U32, bool>& recursive) {
    Array<ModulePtr<Function>> nodes;
    HashMap<U32, U32> index;

    // Sized before the walk rather than grown into. Both tables end up holding one entry per
    // function in the program, and a hash map reached by doubling from empty rehashes its way there
    // - which for a program with a prelude is a dozen allocations and a dozen full rehashes.
    Size functionCount = 0;
    for(auto module: opt.program.modules) functionCount += module->functionOrder.size();

    index.reserve(functionCount);
    recursive.reserve(functionCount);
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
            for(auto instructionPointer: opt.local[blockPointer]->instructions.contents(opt.local)) {
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

                for(auto member: members) recursive.add(U32(nodes[member]), true);
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
     * ## `Move` is in, and it is the one of the ownership four that belongs here
     *
     * The header's rule is that copying an ownership instruction asserts the decision travels. A
     * graft is what *makes* it travel: the whole body is copied, drops and all, and the copy runs
     * once per call exactly as the callee did. What the rule is really about is a decision copied
     * away from the rest of the decision it belongs to, and that is not this.
     *
     * `Move` is additionally not a decision at all by the time this runs. It is the relocation
     * itself - `codegen/js` emits nothing for one but the name, and reads its kind to alias rather
     * than deep-clone - so there is no lower form to discharge it into and nothing left in it to
     * spend. `Drop`, `Swap` and `Exchange` are the ones that still expand into something.
     *
     * The hazard is re-rooting rather than the instruction: a cloned `Move` whose place was rewritten
     * to name the *caller's* storage would empty a slot the caller's ownership state knows nothing
     * about. Two guards already exclude it - the borrow check rejects a move out of a `&` parameter
     * before this stage runs, and a `->` parameter is declined at the site below - and `movesLocal`
     * is the belt that says so in this pass rather than in two others.
     */
    bool clonableKind(Value::Kind kind) {
        switch(kind) {
            case Value::Alloc: case Value::LoadPlace: case Value::Init: case Value::Assign:
            // An array literal, which is one instruction rather than one per element - see
            // InstAggregate. Leaving it out made every function holding a literal un-inlinable,
            // which is how `Array.escaping` stopped folding to its constant.
            case Value::Aggregate:
            case Value::Borrow: case Value::Copy: case Value::Move:
            case Value::TypeMetric: case Value::Symbol:
            case Value::Cast: case Value::Neg: case Value::Not:
            case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
            case Value::Shl: case Value::Shr: case Value::Sar:
            case Value::And: case Value::Or: case Value::Xor: case Value::Cmp:
            // A callee this stage already if-converted. `settle` runs the whole round on a callee
            // before a site is judged against it, so a body reaching here can hold one - and a
            // select is an ordinary pure computation with no decision copied along with it.
            case Value::Select:
            case Value::Call: case Value::Native: case Value::CallDyn:
                return true;
            default:
                return false;
        }
    }

    // The three ways a block can end that this pass knows how to graft. Everything else - a block
    // with no terminator at all, which the resolver only leaves behind on an error path - declines
    // the whole callee, since a body with a way out this does not reproduce is one whose copy would
    // simply fall off the end.
    bool clonableTerminator(Value::Kind kind) {
        return kind == Value::Ret || kind == Value::Jmp || kind == Value::Je;
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
     * Both departure points are asked about, because both name a place: `Move` is the relocation and
     * `Exchange` writes a new value over one it takes out. Neither is reachable today for a
     * re-rooted parameter - the borrow check refuses the `&` case and `describe` refuses the sink
     * case - so this is the statement that they stay unreachable, made where the copy happens rather
     * than in the two passes that currently imply it.
     */
    bool movesLocal(Candidate& candidate, U32 local) {
        auto rootedHere = [&](const Place& place) {
            return place.root == PlaceRoot::Local && place.local == local;
        };

        for(auto blockPointer: candidate.blocks) {
            auto block = opt.local[blockPointer];

            for(auto pointer: block->instructions.contents(opt.local)) {
                auto& instruction = *opt.local[pointer];

                if(instruction.kind == Value::Move &&
                   rootedHere(((InstMove&)instruction).place)) return true;

                if(instruction.kind == Value::Exchange &&
                   rootedHere(((InstExchange&)instruction).place)) return true;
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

            for(auto pointer: block->instructions.contents(opt.local)) visit(*opt.local[pointer]);
            if(block->terminator) visit(*opt.local[block->terminator]);
        }

        return found;
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
                auto successor = block->outgoing[next[next.size() - 1]++];
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

    /*
     * One callee, checked once and described for whatever site is about to copy it.
     *
     * `sink` is set where the site is a `drop` rather than a call. The one thing that changes is the
     * convention rule below: a `->` parameter is ownership transferred at the site, which is exactly
     * what a `drop` performs and never what a call does - see `inlineTeardown`.
     */
    Maybe<Candidate> describe(ModulePtr<Function> pointer, bool sink = false) {
        auto callee = opt.local[pointer];

        // `@noinline`, which is a directive rather than a weight: declining to inline is always
        // possible, so this is the one input to the decision that nothing below can outvote.
        if(callee->noInline) return Nothing();

        if(callee->signature || callee->intrinsic || callee->gen || callee->takesEnv) return Nothing();
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
        if(!sink && taken.get(U32(pointer))) return Nothing();

        // A body that can reach itself, which is unrolling rather than inlining - see the header.
        if(recursive.get(U32(pointer))) return Nothing();

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
        if(entry->phis.isNotEmpty() || entry->incoming.isNotEmpty()) return Nothing();

        for(auto blockPointer: candidate.blocks) {
            auto block = opt.local[blockPointer];
            if(!block->terminator) return Nothing();

            auto kind = opt.local[block->terminator]->kind;
            if(!clonableTerminator(kind)) return Nothing();
            if(kind == Value::Ret) candidate.returns.push(blockPointer);

            for(auto instructionPointer: block->instructions.contents(opt.local)) {
                auto& instruction = *opt.local[instructionPointer];
                if(!clonableKind(instruction.kind)) return Nothing();

                candidate.size++;
            }

            // A phi is an instruction the caller pays for like any other, and on a managed target it
            // is a variable and an assignment on every edge into the join.
            candidate.size += U32(block->phis.size());
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
            if(slot.closureEnv) return Nothing();

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

        auto& first = (InstRet&)*opt.local[opt.local[candidate.returns[0]]->terminator];
        auto returnsValue = first.value != nullptr;

        for(auto blockPointer: candidate.returns) {
            auto& ret = (InstRet&)*opt.local[opt.local[blockPointer]->terminator];

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
             * And it needs there to be exactly one of them. Two `ret`s of two allocations are two
             * callee locals that would both have to be the caller's one slot, and a slot holds the
             * single value its storage came from - so the phi that answers this for a register
             * result has no counterpart here. A callee shaped that way is left as a call.
             */
            if(type && isMemoryType(opt.global, type)) {
                if(candidate.returns.size() != 1) return Nothing();

                auto returned = opt.local[ret.value];
                if(returned->kind != Value::Alloc) return Nothing();

                candidate.resultLocal = ((InstAlloc&)*returned).local;
                if(candidate.resultLocal >= callee->localCount()) return Nothing();
            }
        }

        return Just(::move(candidate));
    }

    // A value the caller handed over as a literal, which is what every rule below means by a
    // constant argument. Deliberately not `constantValueOf`: this asks whether the folder will have
    // something to work with, not what the number is.
    static bool isLiteral(const Value& value) {
        switch(value.kind) {
            case Value::ConstInt: case Value::ConstFloat: case Value::ConstDouble:
            case Value::ConstString:
                return true;
            default:
                return false;
        }
    }

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
        auto answer = false;

        switch(instruction.kind) {
            case Value::ConstInt: case Value::ConstFloat: case Value::ConstDouble:
            case Value::ConstString:
                answer = true;
                break;
            case Value::Cast: case Value::Neg: case Value::Not:
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
            if(!block->terminator) return false;
            if(opt.local[block->terminator]->kind != Value::Je) continue;

            if(!decidedAtCall(((InstJe&)*opt.local[block->terminator]).cond, decided)) return false;
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
     * Whether this call site is worth what the copy costs.
     *
     * The budget is the callee's size against a limit built from what the *call* looks like, and
     * every term is named in `InlinePolicy`. A call site that clears the ceiling is refused whatever
     * else it has going for it.
     */
    bool worthInlining(Candidate& candidate) {
        if(candidate.size > policy.ceiling) return false;

        auto sites = callSites.getValue(U32(candidate.pointer));
        auto count = sites ? sites.unwrap() : U32(0);

        auto limit = I64(policy.budget);
        if(count <= 1) limit += policy.soleCallSite;
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

        if(candidate.resultLocal != Candidate::kNone) limit += policy.memoryResult;
        if(policy.borrowResult && returnsReifiedReference(*candidate.callee)) {
            limit += policy.borrowResult;
        }

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

        if(place.root == PlaceRoot::Local) {
            auto rewritten = false;

            for(Size i = 0; i < candidate.parameters.size(); i++) {
                auto& parameter = candidate.parameters[i];
                if(parameter.binding == Binding::Value || parameter.local != place.local) continue;

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
            case Value::LoadPlace:
                return (Inst*)createInst<InstLoadPlace>(module, function, into, source, name, type,
                                                        place(((InstLoadPlace&)instruction).place));
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
            case Value::Cast: case Value::Neg: case Value::Not: {
                auto& unary = (InstUnary&)instruction;
                return (Inst*)createInst<InstUnary>(module, function, into, source, name, type,
                                                    instruction.kind, value(unary.from));
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
                return (Inst*)createInst<InstJmp>(module, function, into, source, 0, type,
                                                  continuation);
            case Value::Jmp:
                return (Inst*)createInst<InstJmp>(module, function, into, source, 0, type,
                                                  mapBlock(clone, ((InstJmp&)terminator).target));
            case Value::Je: {
                auto& branch = (InstJe&)terminator;
                return (Inst*)createInst<InstJe>(module, function, into, source, 0, type,
                                                 mapValue(clone, branch.cond),
                                                 mapBlock(clone, branch.thenBlock),
                                                 mapBlock(clone, branch.elseBlock));
            }
            default:
                return nullptr;
        }
    }

    /*
     * The caller's block, cut in two at the call.
     *
     * What was in front of the call stays where it is, because that is where the callee's body is
     * about to go. Everything behind it - the remaining instructions, the terminator, and with the
     * terminator every edge the block owned - moves to a fresh block that the callee's returns will
     * jump to.
     *
     * The two halves of an edge have to move together. A successor records where an edge came from
     * twice, once in its predecessor list and once per phi alternative, and both now name a block
     * the edge no longer leaves from - so both are repointed here rather than left for the phi
     * fill-in below, which would only reach the ones the callee happened to write.
     */
    Block* splitBlock(Block& block, Size index) {
        auto& module = *opt.module;
        auto pointer = (ModulePtr<Block>)(&block - opt.local);

        auto continuation = opt.function->addBlock(module, block.name);
        auto continuationPointer = (ModulePtr<Block>)(continuation - opt.local);
        continuation->source = block.source;

        Array<ModulePtr<Inst>> moved;
        for(Size i = index + 1; i < block.instructions.size(); i++) {
            moved.push(block.instructions.get(opt.local, i));
        }

        for(Size i = block.instructions.size(); i-- > index + 1;) {
            block.instructions.remove(opt.local, i);
        }

        for(auto instruction: moved) {
            opt.local[instruction]->block = continuationPointer;
            continuation->instructions.push(opt.program.arena, instruction);
        }

        continuation->terminator = block.terminator;
        if(block.terminator) opt.local[block.terminator]->block = continuationPointer;

        continuation->outgoing[0] = block.outgoing[0];
        continuation->outgoing[1] = block.outgoing[1];
        block.terminator = nullptr;
        block.outgoing[0] = nullptr;
        block.outgoing[1] = nullptr;

        for(auto successor: continuation->outgoing) {
            if(!successor) continue;

            retargetEdge(opt, opt.local[successor], pointer, continuationPointer);
        }

        return continuation;
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

            opt.function->setLocalValue(opt.local, index, ModulePtr<Value>(mapped.unwrap()));
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

        for(auto instructionPointer: body->instructions.contents(opt.local)) {
            auto& instruction = *opt.local[instructionPointer];
            auto cloned = cloneInstruction(clone, candidate, block, instruction);
            if(!cloned) return Nothing();

            *clone.values.add(U32(instructionPointer)).value = U32(cloned - opt.local);
            clone.emitted.push(cloned);
        }

        bindLocals(clone, candidate);
        insertInstructions(opt, block, index, clone.emitted);

        auto& ret = (InstRet&)*opt.local[body->terminator];
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
     *  6. **the block contents**, added at last - `Block::add` is what records a use and an edge, so
     *     nothing before this point is visible to a walk of the IR.
     *
     * Answers the value the call becomes: the one `ret`'s value where there is one such block, and
     * otherwise a phi in the continuation over all of them.
     */
    Maybe<ModulePtr<Value>> spliceControlFlow(Clone& clone, Candidate& candidate, Inst& site,
                                              Block& block, Size index) {
        auto& module = *opt.module;
        auto& function = *opt.function;

        auto continuation = splitBlock(block, index);
        auto continuationPointer = (ModulePtr<Block>)(continuation - opt.local);

        Array<ClonedBlock> cloned;
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
            for(auto phiPointer: opt.local[target.from]->phis.contents(opt.local)) {
                auto& phi = *opt.local[phiPointer];
                auto copy = createInst<InstPhi>(module, function, *target.to, phi.source, phi.name,
                                                phi.type);

                *clone.values.add(U32(phiPointer)).value = U32((ModulePtr<Value>)(copy - opt.local));
                target.phis.push((Inst*)copy);
            }
        }

        for(auto& target: cloned) {
            auto from = opt.local[target.from];

            for(auto instructionPointer: from->instructions.contents(opt.local)) {
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
                                                *opt.local[from->terminator], continuationPointer);
            assertTrue(target.terminator != nullptr);
        }

        for(auto& target: cloned) {
            auto from = opt.local[target.from];
            Size i = 0;

            for(auto phiPointer: from->phis.contents(opt.local)) {
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

        bindLocals(clone, candidate);

        for(auto& target: cloned) {
            for(auto phi: target.phis) target.to->add(module, phi);
            for(auto instruction: target.instructions) target.to->add(module, instruction);
            target.to->add(module, target.terminator);
        }

        /*
         * And what the call becomes.
         *
         * One returning block dominates the continuation on its own, so its value is simply the
         * answer. Several do not - that is the definition of a join - and the phi is what says so.
         * It is built after the terminators rather than with them because its inputs are the blocks
         * those terminators created the edges from.
         */
        Array<PhiInput> results;
        for(auto blockPointer: candidate.returns) {
            auto& ret = (InstRet&)*opt.local[opt.local[blockPointer]->terminator];
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

        continuation->add(module, phi);
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

        auto described = describe(call.callee);
        if(!described) return false;

        auto candidate = described.unwrap();
        if(candidate.callee->args.size() != call.args.size()) return false;

        for(auto argument: call.args.contents(opt.local)) candidate.arguments.push(argument);

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

            auto argument = call.args.get(opt.local, i);
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

        if(!worthInlining(candidate)) return false;
        if(!graft(candidate, block, index, pointer, grafted)) return false;

        // By hand rather than through `eraseInstruction`, which asserts that nothing reads the
        // instruction - true of a call whose result the graft replaced and not of one returning
        // unit, whose place-root uses are recorded on the locals it named.
        for(auto argument: call.args.contents(opt.local)) dropUse(opt, argument, pointer);
        removeFromBlock(block, pointer);

        opt.changed = true;
        return true;
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
     * Four things are declined and each is a case with no single answer here:
     *
     *  - **both halves present.** A `Drop` runs its `drop` and then its `reclaim`, in that order,
     *    and splicing two bodies into one position is two grafts whose second has to land after the
     *    first. Worth doing and not needed yet: a container whose elements owe a drop has ownership
     *    instructions in that half, which `clonableKind` refuses anyway;
     *  - **`releaseStorage`.** The instruction has a job of its own beyond the callee's, so it
     *    cannot simply go;
     *  - **a conditional drop.** `flag` is a drop flag the analyses computed, and honouring it means
     *    building the branch it stands for rather than copying a body;
     *  - **an erased teardown**, which `describe` already refuses along with every other generic
     *    body: what runs is whatever the caller's descriptor holds, and there is no callee to copy.
     */
    bool inlineTeardown(Block& block, Size index, ModulePtr<Inst> pointer, bool& grafted) {
        auto& dropped = (InstDrop&)*opt.local[pointer];

        if(dropped.releaseStorage || dropped.flag != maxLimit<U32>) return false;
        if(dropped.drop && dropped.reclaim) return false;

        auto callee = dropped.drop ? dropped.drop : dropped.reclaim;
        if(!callee) return false;
        if(callee == (ModulePtr<Function>)(opt.function - opt.local)) return false;

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

        if(!graft(candidate, block, index, pointer, grafted)) return false;

        // The drop's own reads: the storage its place was rooted in, and any index in the path. Both
        // are recorded on the values rather than in an operand slot, which is why this is
        // `eachRootValue` and `eachOperand` rather than an argument list.
        eachOperand(opt.local, dropped, [&](ModulePtr<Value> operand) {
            dropUse(opt, operand, pointer);
        });

        eachRootValue(opt, dropped, [&](ModulePtr<Value> storage) {
            dropUse(opt, storage, pointer);
        });

        removeFromBlock(block, pointer);

        opt.changed = true;
        return true;
    }

    // One instruction dropped from its block's list, leaving its use bookkeeping to the caller -
    // which is what the two sites above differ about.
    void removeFromBlock(Block& block, ModulePtr<Inst> pointer) {
        for(Size i = 0; i < block.instructions.size(); i++) {
            if(block.instructions.get(opt.local, i) != pointer) continue;

            block.instructions.remove(opt.local, i);
            break;
        }
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
        // callee's side. A call whose result is storage the caller named is left alone here.
        if(resultSlots.isNotEmpty() && candidate.returns.size() > 1) return false;

        auto resultSlot = resultSlots.size() ? resultSlots[0]
            : site.kind == Value::Call ? ((InstCall&)site).local : maxLimit<U32>;

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
                                                     slot.convention));
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

        if(result && opt.local[pointer]->uses.isNotEmpty()) {
            replaceValue(opt, (ModulePtr<Value>)pointer, result);
        }

        // And the slots that named the site as their storage, which is not a use and so is not
        // something `replaceValue` reaches - a place rooted in one of them is recorded against the
        // *value* the slot holds, and that value is about to stop existing.
        // Lowest last, so that the back edge `setLocalValue` writes - Value::slot, which is what
        // findPlace and backingLocal answer with - names the same slot the scan above took, since
        // both used to read the first match and only one of the two directions can win.
        for(Size i = resultSlots.size(); i-- > 0;) {
            opt.function->setLocalValue(opt.local, resultSlots[i], result);
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

    // A block's index is its position in this list, which is what every walk in opt_flow.cpp
    // assumes - so rewriting the list means renumbering with it.
    void writeBlocks(SmallArray<ModulePtr<Block>, 12>& order) {
        opt.function->blocks.clear();

        U16 index = 0;
        for(auto pointer: order) {
            opt.function->blocks.push(opt.program.arena, pointer);
            opt.local[pointer]->index = index++;
        }
    }

    /*
     * How many places in the program name each function, recomputed per round since inlining is
     * exactly the thing that changes it.
     *
     * A `drop` counts as a site, because it is one: the teardown it names is a call by the time
     * either backend sees it, and `soleCallSite` would otherwise say a teardown reached from one
     * drop and nothing else has *no* sites - which is the largest bonus in the table handed to a
     * body on the strength of a count that is wrong.
     */
    void countCallSites() {
        callSites.clear();

        auto record = [&](ModulePtr<Function> callee) {
            if(!callee) return;

            auto entry = callSites.add(U32(callee));
            *entry.value = entry.existed ? *entry.value + 1 : 1;
        };

        for(auto module: opt.program.modules) {
            for(auto pointer: module->functionOrder.contents(opt.local)) {
                for(auto blockPointer: opt.local[pointer]->blocks.contents(opt.local)) {
                    for(auto instructionPointer: opt.local[blockPointer]->instructions.contents(opt.local)) {
                        auto& instruction = *opt.local[instructionPointer];

                        if(instruction.kind == Value::Call) {
                            record(((InstCall&)instruction).callee);
                        } else if(instruction.kind == Value::Drop) {
                            record(((InstDrop&)instruction).drop);
                            record(((InstDrop&)instruction).reclaim);
                        }
                    }
                }
            }
        }
    }

    bool runFunction(Function& function) {
        opt.function = &function;
        rebuildUses(opt);

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
            for(Size i = 0; i < block->instructions.size();) {
                auto pointer = block->instructions.get(opt.local, i);
                auto kind = opt.local[pointer]->kind;

                if(kind != Value::Call && kind != Value::Drop) {
                    i++;
                    continue;
                }

                auto replaced = kind == Value::Call
                    ? inlineCall(*block, i, pointer, grafted)
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
        rebuildUses(opt);

        optimizeRounds(opt);
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
    findRecursion(opt, inliner.recursive);

    for(Size round = 0; round < kMaxInlineRounds; round++) {
        inliner.countCallSites();
        auto inlined = false;

        for(auto module: opt.program.modules) {
            opt.module = module;

            for(auto pointer: module->functionOrder.contents(opt.local)) {
                auto function = opt.local[pointer];
                if(function->signature || function->blocks.isEmpty()) continue;

                inlined = inliner.runFunction(*function) || inlined;
            }
        }

        if(!inlined) break;
    }
}
