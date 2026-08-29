#include "analyze_pass.h"
#include "generic.h"

/*
 * The four checks.
 *
 * Every one of them is stated over the facts the passes before it computed and computes nothing of
 * its own, which is what makes them one file: a diagnostic here is either a rule written wrong or a
 * fact computed wrong, and the two are now in different files.
 *
 * What is deliberately *not* checked is recorded at the end of analyze.cpp.
 */

/*
 * Where one borrow is live, one row per block.
 *
 * **A borrow's extent is a region of the control-flow graph and not an interval of the instruction
 * numbering**, and the difference is the whole reason this struct exists. `numberFunction` lays the
 * blocks out in reverse postorder, which puts a loop's *exit* in front of its *body* whenever the
 * exit edge was the one explored first - so
 *
 *     while i < 4:            -- block 3, numbered [15, 24)
 *         insert(m, i, i)
 *
 *     match find(m, 1):       -- block 2, numbered [9, 15)
 *
 * numbers the lookup *before* the loop that filled the map. Scanning `[borrow, lastUse]` as a range
 * of indices then walks straight through the loop body and reports the `insert` as conflicting with
 * a borrow that is not live anywhere near it - which it did, for every `find` after a fill, until
 * this replaced it.
 *
 * So the extent is computed the way a live range is: backwards from each use to the definition,
 * through predecessors, marking the blocks the value is actually live in. What that costs is a
 * walk per borrow; what it buys is that the answer no longer depends on which arm of a branch the
 * block numbering happened to visit first.
 */
struct BlockExtent {
    // Live across the block's first instruction, and live across its terminator.
    bool liveIn = false;
    bool liveOut = false;

    // The last instruction in this block that uses the borrow, where one does. A flag rather than a
    // sentinel index, because index zero is a real position.
    bool used = false;
    U32 lastUse = 0;
};

struct BorrowExtent {
    // One row per block, indexed by `Block::index` - which is the block's position in
    // `Function::blocks` and therefore also its position in `Analysis::blockRanges`.
    SmallArray<BlockExtent, 16> blocks;

    U32 defBlock = 0;
    U32 defIndex = 0;
};

/*
 * The value is live out of `from`, and so live in to every block on the way back to its definition.
 *
 * The walk stops at the defining block in both senses: it marks that block live-*out* (control did
 * leave it carrying the value) and does not mark it live-in, because above the definition the value
 * does not exist. That is also what makes a definition inside a loop right - a borrow taken each
 * time round is not live across the back edge into its own block.
 *
 * **`stop` is the block defining the value being walked, which is not always the borrow.** A loan is
 * followed through the values that carry it - a call result, a phi - and each of those is defined
 * somewhere of its own; walking back from a use of the *phi* has to stop at the merge, because above
 * it the phi does not exist and the arm that did not run never held the loan. Stopping at the
 * borrow's block instead reports each arm of `if flag then pick(x, y) else pick(y, x)` as
 * conflicting with the other.
 *
 * `visited` is per walk rather than per borrow for the same reason: a block one carrier stopped at
 * is one the next may have to walk through.
 *
 * Iterative rather than recursive, because the depth is the block graph's and this runs once per use
 * of every borrow in every function.
 */
static void markLiveOut(Analysis& analysis, BorrowExtent& extent, U32 stop,
                        SmallArray<U8, 16>& visited, ModulePtr<Block> from) {
    SmallArray<ModulePtr<Block>, 16> pending;
    pending.push(from);

    while(pending.size()) {
        auto block = analysis.local[pending.pop().unwrap()];
        if(block->index >= extent.blocks.size()) continue;
        if(visited[block->index]) continue;
        visited[block->index] = 1;

        extent.blocks[block->index].liveOut = true;
        if(block->index == stop) continue;

        extent.blocks[block->index].liveIn = true;
        for(auto predecessor: block->incoming(analysis.local)) pending.push(predecessor);
    }
}

/*
 * The slot a borrow-rooted place ultimately names, where the chain stays inside this frame.
 *
 * `Place::inBorrow(p)` says "the storage `p` refers to", and `p` is very often an `InstBorrow` this
 * body made a moment earlier - a `&` argument, a re-borrow of a loan, a `let &d = c`. Following that
 * one hop answers which local it was a borrow *of*, and two different locals are two different
 * pieces of storage whatever the paths into them look like.
 *
 * Answering `maxLimit` for everything else is what keeps this a narrowing rather than a second
 * aliasing rule: a chain that leaves the frame - a borrow of a pointer, a borrow that came in as an
 * argument - is exactly as unknown as it was, and the caller falls back to what it did before.
 *
 * The bound is the frame's own width for `useSlot`'s reason: a chain with no cycle visits each
 * borrow once, so reaching it means a rewrite built one.
 */
static U32 borrowedSlot(Analysis& analysis, Place place) {
    auto bound = analysis.localCount;
    auto root = maxLimit<U32>;

    for(Size step = 0; step <= bound; step++) {
        if(place.root == PlaceRoot::Local) { root = place.local; break; }
        if(place.root != PlaceRoot::Borrow || !place.pointer) return maxLimit<U32>;

        auto& produced = *analysis.local[place.pointer];
        if(produced.kind != Value::Borrow) return maxLimit<U32>;

        place = ((InstBorrow&)produced).place;
    }

    /*
     * And on down `Local::viewOf`, which is the second half of "which storage is this" and the half
     * a slot answers rather than a place. A `[T *n]` element reaches its borrow through a `Flat`
     * descriptor built at the call, so `fa[0]` and `fa[1]` are rooted in two *different* locals and
     * are one container all the same - which is what Reject.Exchange's `fixedArray` asserts and what
     * stopping at the slot would have quietly stopped reporting.
     */
    for(Size step = 0; root != maxLimit<U32> && step <= bound; step++) {
        auto next = analysis.function.localAt(analysis.local, root).viewOf;
        if(next == maxLimit<U32>) break;
        root = next;
    }

    return root;
}

/*
 * Do two places name storage that can overlap?
 *
 * Same root, and one projection path a prefix of the other. `x.a` and `x.b` are disjoint, `x` and
 * `x.a` are not, and that precision is what makes a `&` parameter usable at all - `moveBy` borrows
 * `p.x` and `p.y` in turn and neither is a conflict with the other.
 *
 * Two deliberate conservatisms. A step through a Deref or an Index leads somewhere this analysis
 * cannot name, so from there on the two are assumed to overlap. And places rooted in raw pointers
 * never conflict with anything, including each other: `%T` carries no aliasing information by
 * construction, and reporting on it would be inventing a rule the language says it does not have.
 *
 * A borrow root is neither of those and used to be treated as neither: the three tests below name
 * Pointer, Local and Global, so two borrow-rooted places fell straight through to the projection
 * walk, which compares nothing about the *roots* and answers "prefix" for two empty paths. Every
 * pair of borrows in a frame therefore overlapped. That was unreachable from ordinary source while
 * the only borrow-rooted places were the ones `swap` and `exchange` build, and became reachable the
 * moment `let &d = c` started binding a loan - `eat(eater, own[1..3])` reported the call as a write
 * conflicting with its own second argument. The two are resolved to their slots instead, and two
 * different slots are two different pieces of storage.
 */
static bool placesOverlap(Analysis& analysis, Place lhs, Place rhs) {
    auto base = analysis.local;

    if(lhs.root == PlaceRoot::Borrow || rhs.root == PlaceRoot::Borrow) {
        auto left = borrowedSlot(analysis, lhs);
        auto right = borrowedSlot(analysis, rhs);

        if(left != maxLimit<U32> && right != maxLimit<U32> && left != right) return false;
    }

    if(lhs.root != rhs.root) return false;
    if(lhs.root == PlaceRoot::Pointer) return false;
    if(lhs.root == PlaceRoot::Local && lhs.local != rhs.local) return false;
    if(lhs.root == PlaceRoot::Global && lhs.global != rhs.global) return false;

    auto left = lhs.projections;
    auto right = rhs.projections;
    auto leftContents = left.contents(base);
    auto rightContents = right.contents(base);

    auto leftIterator = leftContents.begin();
    auto rightIterator = rightContents.begin();

    for(Size i = 0; i < min(left.size(), right.size()); i++) {
        auto a = *leftIterator;
        auto b = *rightIterator;
        ++leftIterator;
        ++rightIterator;

        if(a.kind == ProjectionKind::Deref || a.kind == ProjectionKind::Index) return true;
        if(b.kind == ProjectionKind::Deref || b.kind == ProjectionKind::Index) return true;
        if(a.kind != b.kind || a.index != b.index) return false;
    }

    // One path ran out, so it is a prefix of the other and names storage containing it.
    return true;
}

/*
 * The container a pointer-rooted borrow reaches into, where there is one this frame can name.
 *
 * `xs[i]` in a `&` position is `Index.getMut`, whose body is `borrowMut(self.run.items + index)` -
 * so what arrives at the borrow checker is `borrow_mut [%p]` with `%p` an offset from a base
 * pointer *loaded out of the container's own storage*. placesOverlap() answers no for two of those,
 * by the rule that a raw pointer carries no aliasing information, and that rule is right about a
 * pointer and wrong about this: the two addresses were derived from one place a moment earlier, and
 * that place is checkable.
 *
 * So the derivation is walked back rather than the pointer reasoned about. Through the offset
 * arithmetic to the load that produced the base, and through the load's own root to the local the
 * container lives in - which is the whole chain, since the accessor is one expression.
 *
 * Both halves of the answer are needed and neither is enough alone. The local tells two containers
 * apart, so `f(&a[i], &b[j])` stays legal; the loaded place keeps the field sensitivity
 * placesOverlap already has, so a container with two runs is two containers here as well.
 *
 * Nothing found is the honest answer for storage this frame did not name - a `%T` parameter, a
 * pointer out of an opaque call - and it leaves those exactly as unchecked as they were. That is
 * the seam analyze.cpp's limitation list describes, kept where it was put: a collection written
 * over raw storage is still trusted about aliasing *inside itself*.
 */
struct ContainerReach {
    bool found = false;
    U32 local = maxLimit<U32>;
    Place field;
};

static ContainerReach containerReach(Analysis& analysis, const Place& place) {
    ContainerReach result;
    if(place.root != PlaceRoot::Pointer || !place.pointer) return result;

    auto value = place.pointer;

    for(Size step = 0; step < 8 && value; step++) {
        auto& produced = *analysis.local[value];

        // The index, folded into the address. Which operand is the base is read off the types
        // rather than assumed: `p + i` is the shape the accessor writes, and the addition is
        // commutative even where the meaning is not.
        if(produced.kind == Value::Add || produced.kind == Value::Sub) {
            auto& arithmetic = (InstBinary&)produced;
            auto lhs = arithmetic.lhs ? analysis.local[arithmetic.lhs]->type : nullptr;
            value = lhs && isPointer(analysis.global, lhs) ? arithmetic.lhs : arithmetic.rhs;
            continue;
        }

        if(produced.kind != Value::LoadPlace) return result;

        auto loaded = ((InstLoadPlace&)produced).place;
        auto local = rootLocal(analysis, loaded);

        // A container reached through a `&` of it - which is every call of an accessor, since the
        // receiver is `return &self`. One step is enough: the borrow's own place is the container.
        if(local == maxLimit<U32> && loaded.root == PlaceRoot::Borrow && loaded.pointer) {
            auto& source = *analysis.local[loaded.pointer];
            if(source.kind == Value::Borrow) local = rootLocal(analysis, ((InstBorrow&)source).place);
        }

        if(local == maxLimit<U32>) return result;

        /*
         * And through a descriptor to what it describes - Local::viewOf, the same chain useSlot
         * walks. A `[T *n]` reaches a subscript as a `Flat(T)` built at the call, so two subscripts
         * of one fixed array are two descriptors and would otherwise be two containers. The depth
         * bound is against a cycle, exactly as it is there.
         */
        for(Size hop = 0; hop < 8; hop++) {
            auto view = analysis.function.localAt(analysis.local, local).viewOf;
            if(view == maxLimit<U32> || view >= analysis.localCount) break;
            local = view;
        }

        result.found = true;
        result.local = local;
        result.field = loaded;
        return result;
    }

    return result;
}

// Two exclusive borrows that reach into one container, which cannot be told apart by index without
// proving something about the two subscripts - and proving that is a different analysis than this.
static bool sameContainer(Analysis& analysis, const Place& lhs, const Place& rhs) {
    auto left = containerReach(analysis, lhs);
    if(!left.found) return false;

    auto right = containerReach(analysis, rhs);
    if(!right.found || left.local != right.local) return false;

    return placesOverlap(analysis, left.field, right.field);
}

/*
 * Whether a borrow is handed to nothing but an operation that is total under aliasing.
 *
 * `swap` and `exchange` are the two, and being usable on two elements of one container is what they
 * are *for*: a swap reads both places before it writes either, so the two naming one place is a
 * no-op rather than a loss, and an exchange has one place to begin with. That is why the library
 * had a `swapElements` at all before a subscript could reach a `&` parameter.
 *
 * The exemption is per borrow and not per call, which is the narrow way to say it: a borrow that
 * reaches anything else - a user function, a store, a second name - is not covered by the argument
 * above and is checked. A borrow with no uses at all is not exempt either, since the reason to
 * excuse it would be missing.
 */
static bool exchangedOnly(Analysis& analysis, ModulePtr<Value> borrow) {
    auto any = false;

    for(auto user: analysis.local[borrow]->uses(analysis.local)) {
        auto kind = analysis.local[user]->kind;
        if(kind != Value::Swap && kind != Value::Exchange) return false;
        any = true;
    }

    return any;
}

/*
 * Exclusivity - Design.md's second question, and the only one of the four with an extent to
 * compute first.
 */

/*
 * How far a borrow's extent reaches.
 *
 * To the last instruction that consumes the borrow value - and then, if one of those was a call
 * that may hand it back, to the last use of what the call produced. That second clause is the
 * whole of Design.md's "the caller conservatively keeps every member borrowed until the last use
 * of all result borrows", and it is why the loan on `objects` does not end at the call to
 * `getMutableEntry` but at the last use of the entry it returned.
 *
 * Transitive, because a caller may hand the result on again: a chain of selectors keeps the
 * original root borrowed for the whole chain. The `seen` list is what makes a value used by two
 * calls, or a loop, terminate instead of walking the graph forever.
 *
 * Which of those uses are reached is one question and *where the value is live on the way to them*
 * is another, and this answers both: every use found below is marked into `extent`, from which the
 * blocks the borrow is live in follow by walking predecessors. See BorrowExtent for why an interval
 * of instruction indices is not an answer to the second.
 */
static void computeExtent(Analysis& analysis, ModulePtr<Inst> pointer, BorrowExtent& extent) {
    extent.blocks.clear();
    for(Size b = 0; b < analysis.blockCount(); b++) extent.blocks.push(BlockExtent());

    auto& borrow = *analysis.local[pointer];
    extent.defBlock = analysis.local[borrow.block]->index;

    auto found = analysis.indexOf.get(U32(pointer));
    extent.defIndex = found ? found.unwrap() : 0;

    ValueList pending;
    ValueList seen;
    pending.push((ModulePtr<Value>)pointer);

    while(pending.size()) {
        auto value = pending.pop().unwrap();

        auto walked = false;
        for(auto& entry: seen) walked = walked || entry == value;
        if(walked) continue;
        seen.push(value);

        /*
         * The block this carrier is defined in, which is what its own uses are walked back to - see
         * markLiveOut. The borrow is one carrier among several and has no special standing here.
         */
        auto carrierBlock = analysis.local[analysis.local[value]->block]->index;

        SmallArray<U8, 16> visited;
        for(Size b = 0; b < extent.blocks.size(); b++) visited.push(0);

        for(auto user: analysis.local[value]->uses(analysis.local)) {
            auto& instruction = *analysis.local[user];

            /*
             * Where this use puts the value.
             *
             * A phi is the one user that does not make the value live in its own block: an operand
             * arrives along one edge, so what it says is that the value is live *out of that
             * predecessor* and nothing about the other arms. Marking the phi's block live-in
             * instead would keep a borrow alive down every path into a merge, which is how a value
             * that a branch discarded ends up conflicting with the branch that discarded it.
             *
             * Every other user makes the value live at its own position, and live in to its block
             * unless that block is where the borrow was taken - in which case it is live from the
             * borrow onwards and no further back.
             */
            if(instruction.kind == Value::Phi) {
                auto& phi = (InstPhi&)instruction;
                for(Size input = 0; input < phi.inputs.size(); input++) {
                    auto arm = phi.inputs.get(analysis.local, input);
                    if(arm.value == value && arm.block) {
                        markLiveOut(analysis, extent, carrierBlock, visited, arm.block);
                    }
                }

                /*
                 * And the phi carries the loan on, which is the other half of the same fact: a merge
                 * of two borrows may name either, so a use of the merged value keeps *both* alive.
                 * Without this the extent stopped dead at the phi - `let &chosen = if flag then
                 * pick(x, y) else pick(y, x)` followed by a write to `x` was accepted in silence,
                 * because nothing after the merge was reachable from either borrow.
                 */
                pending.push((ModulePtr<Value>)user);
            } else if(auto index = analysis.indexOf.get(U32(user))) {
                auto block = analysis.local[instruction.block];

                if(block->index < extent.blocks.size()) {
                    auto& row = extent.blocks[block->index];

                    if(!row.used || index.unwrap() > row.lastUse) {
                        row.used = true;
                        row.lastUse = index.unwrap();
                    }

                    if(block->index != carrierBlock) {
                        row.liveIn = true;
                        for(auto predecessor: block->incoming(analysis.local)) {
                            markLiveOut(analysis, extent, carrierBlock, visited, predecessor);
                        }
                    }
                }
            }

            /*
             * A borrow written into a closure's environment is live for as long as the closure is.
             *
             * Design-Memory §8: "a by-reference capture is a mutable borrow live for as long as the
             * closure, so while such a closure exists the captured binding may not be borrowed again
             * by the enclosing frame". The extent therefore follows the storage the borrow was
             * written into, then the address of that storage, then the function value the address
             * became a word of - which is exactly the chain a capture takes to reach a `calldyn`.
             */
            Place carrier;
            auto storedInto = instruction.kind == Value::Init && firstPlace(instruction, carrier);
            auto derivedFrom = (instruction.kind == Value::Address || instruction.kind == Value::Borrow) &&
                               firstPlace(instruction, carrier);

            /*
             * A raw pointer read out of borrowed storage is live for as long as whatever holds it.
             *
             * Implementation-Containers.md §4: borrowing `[T]` copies the run's base address and the
             * length into a descriptor, and hands *that* over. The borrow's own last use is the read
             * - so without this the loan would end before the call that is about to index through
             * the address it produced, and the drop pass would be entitled to release the run first.
             *
             * Narrow on purpose. Only a *pointer* read through the borrow extends it, because only a
             * pointer still names the storage after the read; an `Int` copied out of a borrowed
             * record is a value with no relationship to where it came from, and following that would
             * keep every field read's owner borrowed to the end of the function.
             */
            auto aliases = instruction.kind == Value::LoadPlace &&
                           isPointer(analysis.global, instruction.type);

            if(aliases) pending.push((ModulePtr<Value>)user);

            if(storedInto || derivedFrom) {
                auto root = rootLocal(analysis, carrier);

                if(root != maxLimit<U32>) {
                    auto slot = analysis.function.localAt(analysis.local, root);

                    // Into an environment, or into the function value the environment ends up in -
                    // and into anything at all when what is written is one of the aliasing pointers
                    // above, which is how the slice descriptor carries the loan to its call site.
                    // The rule is `carriesLoanOnward`, shared with the liveness half - see there.
                    auto carries = carriesLoanOnward(analysis, slot, analysis.local[value]->type);

                    if(storedInto && carries && slot.value) pending.push(slot.value);

                    // Out of an environment: the address is what the function value holds.
                    if(derivedFrom && slot.closureEnv) pending.push((ModulePtr<Value>)user);
                }
            }

            /*
             * A loan handed to a call's `return` group outlives the call, whichever way the callee
             * was named.
             *
             * A direct call reads the group off the callee's summary and a call through a function
             * value reads it off the signature, which is the same contract in the two places it can
             * be written down - and the reason FunArg carries the marker at all. A chain of
             * selectors keeps the original root borrowed for the whole chain either way.
             *
             * Which group, and not merely whether there is one - Analysis-Borrows.md §4.2. A
             * signature that split its loans said that a result in one group is a loan of that
             * group's members and of nothing else, so stretching every marked argument's extent to
             * this result would be reading the contract and then ignoring half of it. `resultGroup`
             * is `kNoLoan` where the result is not itself a reference, and that means "every group"
             * rather than "none" - the same field-insensitivity, and the same answer, as
             * dynamicResultProvenance.
             */
            U64 roots = 0;
            LoanGroup argumentGroups[64];
            bool argumentDestination[64];
            auto groupsKnown = false;

            if(instruction.kind == Value::Call) {
                auto summary = summaryOf(analysis, ((InstCall&)instruction).callee);

                if(summary) {
                    roots = summary->declaredRoots;

                    auto declared = analysis.local[((InstCall&)instruction).callee];
                    U16 index = 0;

                    for(auto argPointer: declared->args.contents(analysis.local)) {
                        if(index >= 64) break;

                        auto declaredArg = analysis.local[argPointer];
                        argumentDestination[index] = declaredArg->isMutableBorrow();
                        argumentGroups[index++] = declaredArg->loan;
                    }

                    for(auto i = index; i < 64; i++) {
                        argumentGroups[i] = kNoLoan;
                        argumentDestination[i] = false;
                    }

                    groupsKnown = true;
                }
            } else if(instruction.kind == Value::CallDyn) {
                auto signature = ((InstCallDyn&)instruction).signature;

                if(signature && analysis.global[signature]->kind == Type::Fun) {
                    auto type = (FunType*)analysis.global[signature];
                    roots = type->returnRoots;

                    U16 index = 0;
                    for(; index < type->args.size() && index < 64; index++) {
                        auto declaredArg = type->args.get(analysis.global, index);
                        argumentGroups[index] = declaredArg.loan;
                        argumentDestination[index] = declaredArg.convention == ast::BindType::Ref;
                    }

                    for(auto i = index; i < 64; i++) {
                        argumentGroups[i] = kNoLoan;
                        argumentDestination[i] = false;
                    }

                    groupsKnown = true;
                }
            }

            if(!roots) continue;

            auto resultType = analysis.local[(ModulePtr<Value>)user]->type;
            auto resultGroup = resultType && analysis.global[resultType]->kind == Type::Borrow
                ? ((BorrowType*)analysis.global[resultType])->loan : kNoLoan;

            U16 position = 0;
            auto args = instruction.kind == Value::Call ? &((InstCall&)instruction).args
                                                        : &((InstCallDyn&)instruction).args;

            for(auto arg: args->contents(analysis.local)) {
                auto inGroup = !groupsKnown || resultGroup == kNoLoan || position >= 64 ||
                               argumentGroups[position] == resultGroup;

                if(arg != value || !(roots & rootBit(position))) {
                    position++;
                    continue;
                }

                if(inGroup) pending.push((ModulePtr<Value>)user);

                /*
                 * And to the other arguments in the same group - Analysis-Borrows.md §4.7.
                 *
                 * Returning a reference is only one way a loan can outlive a call. A function can
                 * also install one in a caller-owned destination:
                 *
                 *     fn remember(&store: Store(kept'), value: kept'Cell) -> {}
                 *
                 * There is no result here for the loan to be bounded by, and bounding it by the
                 * call is exactly wrong - the whole point of the signature is that what `store`
                 * holds afterwards is a reference to `value`. What bounds it is the *destination*,
                 * so the loan runs to the last use of every other argument in its group, which is
                 * the same relation a result in the group already had.
                 *
                 * Into a **destination** only, which is the asymmetry §4.7 is about and not a
                 * refinement of it. `pick(x, y, flag) -> &T` has two co-members of one group and
                 * neither installs into the other - they are both *sources*, and the result is the
                 * destination, which the arm above already covers. Extending between them would
                 * make `x`'s loan cover `y`'s extent and reject reading `y` afterwards, which
                 * `DropAssignBorrow.yana` does and should go on doing. So the loan flows from a
                 * source to a `&` destination and nowhere else.
                 *
                 * The group is what keeps even that from being every `&` argument:
                 * `remember(box, source)` extends `source`'s loan over `box`'s uses and over
                 * nothing else, and a parameter outside the group is unaffected. A signature that
                 * wanted two independent destinations splits them with labels (§4.2).
                 */
                if(groupsKnown && position < 64 && argumentGroups[position] != kNoLoan &&
                   !argumentDestination[position]) {
                    U16 other = 0;

                    for(auto peer: args->contents(analysis.local)) {
                        if(other == position || other >= 64 || !argumentDestination[other] ||
                           argumentGroups[other] != argumentGroups[position]) {
                            other++;
                            continue;
                        }

                        /*
                         * The peer's *root local*, and not the peer.
                         *
                         * What is passed at a destination position is a borrow built for the call,
                         * and that temporary dies with the call - extending over it would add
                         * nothing and the loan would still end where it started. What has to stay
                         * borrowed is the storage the destination names, so the walk continues from
                         * the local the argument was borrowed out of, and the loan runs as far as
                         * that local's own last use.
                         */
                        auto& borrowed = *analysis.local[peer];
                        auto root = borrowed.kind == Value::Borrow
                            ? rootLocal(analysis, ((InstBorrow&)borrowed).place)
                            : backingLocal(analysis, peer);

                        if(root == maxLimit<U32>) { other++; continue; }

                        auto slot = analysis.function.localAt(analysis.local, root);
                        if(slot.value) pending.push(slot.value);

                        other++;
                    }
                }

                position++;
            }
        }
    }
}

void checkBorrows(Analysis& analysis) {
    // One extent, refilled per borrow. It is a walk of the block graph, so what it holds is
    // proportional to the function rather than to the borrow, and there is nothing in it worth
    // building twice.
    BorrowExtent extent;

    for(Size at = 0; at < analysis.instructionCount; at++) {
        auto pointer = analysis.order[at];
        auto& borrowed = (InstBorrow&)*analysis.local[pointer];
        if(borrowed.kind != Value::Borrow) continue;

        computeExtent(analysis, pointer, extent);

        /*
         * Block by block rather than straight down the numbering, which is the fix - see
         * BorrowExtent. What is scanned inside each block is the part of it the borrow is actually
         * live across:
         *
         *  - from the top where the borrow reaches the block along an edge, and from the borrow
         *    itself in the block that took it;
         *  - to the bottom where it leaves along an edge, and to its last use in the block where
         *    it dies.
         *
         * A block it is live neither in nor out of and uses in nowhere is not scanned at all, which
         * is every block of a loop the borrow was taken after.
         */
        for(Size b = 0; b < analysis.blockCount() && b < extent.blocks.size(); b++) {
            auto& row = extent.blocks[b];
            auto defines = b == extent.defBlock;
            if(!row.liveIn && !row.liveOut && !row.used && !defines) continue;

            auto& range = analysis.blockRanges[b];
            auto first = row.liveIn || !defines ? range.first : extent.defIndex + 1;
            auto end = row.liveOut ? range.end : (row.used ? row.lastUse + 1 : first);

            for(Size i = first; i < end; i++) {
                auto other = analysis.order[i];
                auto& instruction = *analysis.local[other];

                Place places[kMaxPlaces];
                auto touched = instructionPlaces(instruction, places);
                if(!touched) continue;

                auto overlaps = false;
                for(Size p = 0; p < touched; p++) {
                    overlaps = overlaps || placesOverlap(analysis, borrowed.place, places[p]);
                }

                /*
                 * And the case placesOverlap() is entitled to answer no for: two elements of one
                 * container, whose addresses are raw pointers by the time a borrow is taken of
                 * them. Asked only of two *exclusive* borrows, which is the pair that cannot both
                 * be right - a shared one alongside another shared one is what borrows are for,
                 * and a shared one alongside an exclusive one is already caught wherever the
                 * container itself is named.
                 */
                auto container = false;

                if(!overlaps && borrowed.mut && instruction.kind == Value::Borrow &&
                   ((InstBorrow&)instruction).mut) {
                    container = sameContainer(analysis, borrowed.place, ((InstBorrow&)instruction).place);
                }

                if(!overlaps && !container) continue;

                // The two operations that mean it - see exchangedOnly. Both ends have to be one,
                // since it is the pair that is being excused rather than either borrow.
                if(container && exchangedOnly(analysis, (ModulePtr<Value>)pointer) &&
                   exchangedOnly(analysis, (ModulePtr<Value>)other)) {
                    continue;
                }

                // The instructions that consume the borrow reach the storage *through* it, which is
                // the whole point of handing one out rather than a conflict with it.
                auto consumed = false;
                for(auto user: analysis.local[pointer]->uses(analysis.local)) {
                    if(user == other) consumed = true;
                }

                if(consumed) continue;

                auto otherBorrow = instruction.kind == Value::Borrow;
                auto otherMutable = otherBorrow && ((InstBorrow&)instruction).mut;

                // Two immutable borrows of one place are exactly what borrows are for.
                if(!borrowed.mut && otherBorrow && !otherMutable) continue;

                // Reading through a live immutable borrow is fine; it is the mutable one that is
                // exclusive. A write is a conflict with either.
                auto writes = instruction.kind == Value::Assign || instruction.kind == Value::Init ||
                              instruction.kind == Value::Move || otherMutable ||
                              instruction.kind == Value::Address ||
                              instruction.kind == Value::Swap || instruction.kind == Value::Exchange;

                if(!borrowed.mut && !writes) continue;

                report(analysis,
                       container
                           ? "two exclusive borrows of elements of one container are live at once, and nothing here says the two subscripts differ - `swap` and `exchange` are the operations that need no such proof"_v
                           : borrowed.mut
                               ? "this use conflicts with a mutable borrow of the same storage, which is exclusive while it is live"_v
                               : "this write conflicts with an immutable borrow of the same storage that is still live"_v,
                       instruction.source);

                note(analysis, "the borrow it conflicts with is here"_v, borrowed.source);
            }
        }
    }
}

/*
 * Whether a move out of this place empties the slot rather than half of it.
 *
 * The empty path is the obvious one. The other is a lone `Downcast`, and it is not an exception to
 * the rule so much as the case where the rule's premise does not hold: a record's storage is a
 * discriminant and one payload, the discriminant owns nothing, and on the path a `Just(->v)` reached
 * there is no second payload to be left behind. So the slot really is empty afterwards, and every
 * later use of it is rejected and every drop of it skipped, which is what "moved" has to mean.
 *
 * Two steps do not qualify, and both matter. `Downcast` then `Field` is one member of a payload
 * written with braces, which is a genuine partial move. `Downcast` then `Deref` is a boxed payload -
 * `project` appends that `Deref` - and there the box is storage of its own: taking the target out
 * leaves the allocation with nothing to free it, since the reclaim that would have is the one this
 * move just told the compiler not to run.
 */
static bool wholeMove(Analysis& analysis, Place place) {
    auto count = place.projections.size();
    if(count == 0) return true;
    if(count > 1) return false;

    return place.projections.get(analysis.local, 0).kind == ProjectionKind::Downcast;
}

/*
 * What storage a departing operand names, and whether this frame owns it.
 *
 * The one question checkTransfer asks. It used to be asked twice, because a handover reaches a
 * departure point in two shapes and each half derived the answer its own way - the place half from
 * the root, the other from `backingLocal` - which is §0's shape exactly, and it had already gone
 * wrong once in the way recorded above `checkTransfer`.
 *
 * `place` is the only thing the two shapes may still differ by afterwards, and what it decides is
 * which diagnostic is right rather than whether there is one. See the reports.
 */
struct TransferSource {
    // The tracked slot, where there is one. `maxLimit<U32>` covers both a root that is not a local
    // at all and a value with no slot behind it, which is why `owned` is recorded beside it rather
    // than derived from it: a borrow root has no local and is not owned, and a fresh call result
    // has no local and is.
    U32 local = maxLimit<U32>;

    // Whether this frame is the one responsible for releasing what the operand names.
    bool owned = false;

    // A raw pointer's target - `%T` carries no owner, so nothing here has an opinion about it.
    bool outsideModel = false;

    // Set where the operand is a read of a place, and null where it is the value that produced an
    // aggregate. Not "which shape was it" for its own sake: a place can be told to write `->` on
    // the binding that names it, and a borrowed parameter travelling as its `Arg` has no binding to
    // write it on.
    Place* place = nullptr;
};

static TransferSource transferSource(Analysis& analysis, ModulePtr<Value> value) {
    TransferSource result;

    /*
     * The handover reached without a load at all.
     *
     * An aggregate parameter travels as its `Arg` rather than as a read of a place - the rule
     * analyze_effects states as "aggregates travel through the IR as the value that produced them"
     * - so `fn f(v: Held, &out: Held): out = v` is `assign %out, %v` with no LoadPlace anywhere.
     * transferFrom does find the slot and marks it moved, which is right for a slot this frame owns
     * and worse than useless for one it does not: the caller still owns what it lent, and the
     * destination now owns it too.
     *
     * A value with no slot behind it owns itself - a call result the resolver did not give storage,
     * a construction - so there is nothing here for this frame not to own.
     */
    if(analysis.local[value]->kind != Value::LoadPlace) {
        result.local = backingLocal(analysis, value);
        result.owned = result.local == maxLimit<U32> || analysis.tracked[result.local].owned;
        return result;
    }

    auto& place = ((InstLoadPlace*)analysis.local[value])->place;
    result.place = &place;

    /*
     * A raw pointer's target has no owner, so nothing here has an opinion about *whose* it is - but
     * it is still a value, and duplicating one with a teardown is still duplicating it.
     *
     * `owned` is set for exactly that reason. The three "this frame only borrows it" messages below
     * are about a lender, and a pointer has none to name; the copy check under them is about the
     * value, and it applies. Leaving `owned` false sent a pointer target down a branch whose
     * sentence would have been false and out the other side unchecked.
     */
    if(place.root == PlaceRoot::Pointer) {
        result.outsideModel = true;
        return result;
    }

    result.local = rootLocal(analysis, place);
    result.owned = place.root != PlaceRoot::Borrow && place.root != PlaceRoot::Global &&
                   (result.local == maxLimit<U32> || analysis.tracked[result.local].owned);
    return result;
}

/*
 * Where a slot handed on at `from` is read again, for the note beside P3's diagnostic.
 *
 * Forwards through the numbering from the departure, which is the reading order for everything but a
 * back edge - and a use reached only round a loop answers `kNullLocation`, which prints the error
 * with no note rather than pointing at a line above the one it is about. A guess would be worse than
 * silence here: the whole purpose of the note is to name the use the reader has to find.
 */
static LocationId nextUseAfter(Analysis& analysis, U32 root, Size from) {
    if(root == maxLimit<U32>) return kNullLocation;

    for(auto i = from + 1; i < analysis.instructionCount; i++) {
        auto& effects = analysis.effects[i];

        auto reads = false;
        for(auto use: effects.uses) reads = reads || use == root;
        for(auto overwritten: effects.overwrites) reads = reads || overwritten == root;
        for(auto joined: effects.joins) reads = reads || joined == root;

        if(reads) return analysis.local[analysis.order[i]]->source;
    }

    return kNullLocation;
}

/*
 * Ownership leaving the frame through a value that was never moved out of anything.
 *
 * The four points where ownership departs - a write into another place, an exchange, a return, and a
 * phi input - all take a *value*, and transferFrom stops at whichever slot that value is the whole
 * contents of. A projected load has no such slot: `p.first` and the `v` a `Held(v)` pattern binds are
 * both a LoadPlace over a path into a slot somebody else still owns, so the handover finds nothing to
 * mark moved, the source is dropped at its last use, and whoever received the bytes drops them too.
 *
 * That is the same statement `InstMove` makes with a projected place, and it is refused for the same
 * reason - a slot half given away needs a drop flag per field, which the state lattice does not have.
 * The only difference is that `->` says it out loud and this does not, so this is where it has to be
 * noticed. Written as a check rather than as a move: turning these into InstMove would make the
 * *accepted* programs depend on a rule that exists to reject, and a partial move is not something to
 * start representing here.
 *
 * Only droppable types, on the same terms as transferFrom itself. Copying the bytes of something
 * nobody is responsible for is a copy, and that is all `let x = p.count` has ever been.
 *
 * The *unprojected* load was skipped here on the reading that transferFrom would have caught it -
 * "whichever slot that value is the whole contents of". It does not: backingLocal matches a slot
 * whose defining value *is* the operand, which is true of a call result or a construction and false
 * of a load. So `b = a` for two owned `Held` locals read `a`, dropped `b`'s old contents, aliased
 * `a`'s into `b`, and then dropped both names - three drops for two objects.
 *
 * One rule for all four departure points, which `eachTransferOperand` is the list of. It briefly
 * had a flag saying which of them it applied to; every caller passed the same answer, so what the
 * flag described was a distinction that had already stopped existing.
 *
 * And one derivation of the rule's one input, which `transferSource` is. The two shapes a handover
 * arrives in used to answer "does this frame own what is departing" separately, in opposite orders,
 * and the three-drops bug above is what a disagreement between them looks like: it is not that one
 * half was wrong, it is that there were two halves to keep right. What survives the merge is the
 * only difference that was ever load-bearing - whether there is a *place* the reader can be pointed
 * at - and it decides which advice is printed rather than whether anything is.
 */
static void checkTransfer(Analysis& analysis, ModulePtr<Value> value, LocationId source, Size at) {
    if(!value) return;

    /*
     * A departure `computeLastUseMoves` already decided is a move - Design-Test.md §11.1's P3.
     *
     * Ahead of everything below because it is not an exemption from those rules but an answer to the
     * question they ask: this frame owns the value, the name it read is never read again, and the
     * ownership lattice has been told the slot is empty from here on. There is nothing left for a
     * copy check to be about.
     *
     * The decision is read rather than re-derived. It is the same rule twice otherwise - one copy
     * here and one in the pass that writes the effect - and the two would then be free to disagree
     * about which departures move, which is the failure mode this whole file is written against.
     */
    if(analysis.lastUseMoves[analysis.local[value]->id]) return;

    auto from = transferSource(analysis, value);

    /*
     * A raw pointer's target, which has no owner - so the three "this frame only borrows it"
     * messages below have no lender to name and are skipped. The **copy** check under them is a
     * different question and is not skipped, because duplicating a value with a teardown is about
     * the value rather than about whose it was.
     *
     * All of it used to leave here unchecked, on the reading that `return *p` is what Native needs
     * it to be. What that exempted was not the *move* - `let ->x = *p` is an InstMove and never
     * reaches this function at all, which is what `Reclaim(Array(a))` is written on - but the copy.
     * So `fn read(p: %String) -> String = *p` handed the caller a string to release while the memory
     * kept the only other reference to the same run, and `let &y = *p` made a second owner of it in
     * one line, both silently. The identical line over a local has been refused since before
     * pointers had a section of their own.
     *
     * The escape hatch is not closed, it is spelled: the diagnostic names `let ->`, that spelling
     * still means what it always meant, and Native has to say which of the two it is doing. Nothing
     * in the library had to change.
     *
     * The whole of what the pointer names, and only that. A projection through one leaves the root
     * behind - the same line placeOverwriteDrops draws - so what is on the other side is not a value
     * this can speak for, and the fall-through it would reach is "move the whole value instead",
     * which for the environment of a `@lazy` thunk is a sentence about a projection nobody wrote.
     */
    if(from.outsideModel) {
        if(!from.place) return;

        auto projections = from.place->projections;
        if(projections.isNotEmpty()) return;

        auto held = ownershipIn(analysis.module, functionGen(analysis.global, analysis.function),
                                analysis.local[value]->type);
        if(!held.needsTeardown()) return;

        report(analysis, "this copies the bytes of a value with a teardown into another place, so both names would run it - bind it with `let ->` to move it instead, or use `swap` or `exchange` to write into a place that already holds one"_v,
               source);
        return;
    }

    // Asked of the *context* rather than of the type, exactly as sinkValue asks it: an unconstrained
    // `a` owns something inside the body whatever a caller substitutes, and a declared
    // `TrivialCopy(a)` is what makes reading one out a copy. Without that agreement a generic
    // accessor would be rejected here and then compile the copy anyway.
    auto ownership = ownershipIn(analysis.module, functionGen(analysis.global, analysis.function),
                                 analysis.local[value]->type);
    if(!ownership.needsTeardown()) return;

    /*
     * Storage this frame only borrows, handed on to somebody who will release it.
     *
     * Two messages for one rule, and the difference is what the reader can do about it. An operand
     * that is a read of a place has a binding to put `->` on or a borrow to answer instead; one
     * that is a borrowed parameter travelling as its own `Arg` has neither, and the only honest
     * advice is to change the signature.
     *
     * Nothing here ever looks at an *argument*: handing a borrowed value on as one re-borrows it -
     * another immutable borrow, or the one mutable borrow forwarded - and an argument is not a
     * departure point, which is what eachTransferOperand is the list of.
     */
    if(!from.owned) {
        if(from.place) {
            report(analysis, "this hands ownership on out of storage this frame only borrows - take the whole value with `->` in the signature, or answer a borrow instead"_v,
                   source);
        } else {
            report(analysis, "this stores a value this frame only borrows, so the caller and this destination would both run its teardown - take it with `->` in the signature to own it here"_v,
                   source);
        }

        return;
    }

    // A slot this frame owns, departing as the value that produced it. That *is* the move, and
    // transferFrom marks it moved; there is nothing to report and no place to report it about.
    if(!from.place) return;

    auto& place = *from.place;

    /*
     * The whole slot, handed on - and reached only where the name is read again below, because a
     * last use is now the move it looks like (P3, and the check at the top of this function).
     *
     * So the message is about the *later* use and not about the departure, which is the whole of
     * what changed here: the line the reader is looking at is fine, and what makes it a copy is
     * something further down. Both are named, because a rule that depends on liveness is a rule
     * whose second half is invisible - adding a use below can turn a working move into an error at
     * a line the author did not touch, and pointing only at the departure leaves them hunting for
     * the use that caused it.
     *
     * **`Copy` is named now, where it deliberately was not.** The objection was that it is authored
     * per type and most droppable types do not have one, so naming it would send readers looking for
     * something usually absent. The blanket `instance (TrivialCopy(a)) Copy(a)` overtook that, along
     * with instances for `String`, `Array`, `Map`, `Entry`, `Maybe`, `Result` and `Outcome`, so
     * `copy()` is the right word and is usually present.
     */
    if(place.projections.isEmpty()) {
        report(analysis, "this hands on a value with a teardown out of a name that is read again below, so both names would run it - write `copy()` here to duplicate it deliberately, or let this be the last use of the name and it is a move"_v,
               source);

        auto later = nextUseAfter(analysis, from.local, at);
        if(later != kNullLocation) note(analysis, "the name is read again here"_v, later);
        return;
    }

    /*
     * A payload the pivot is the only owner of: the move is representable, it just was not written.
     * Pointing at the spelling is the whole value of separating this case out.
     *
     * Both spellings, which is the other half of Analysis-Language.md §2c. `->` alone gives an
     * immutable binding, so a reader who needs to write through the name afterwards would be sent
     * straight back here by the next diagnostic; `&->` is the same move into storage this binding
     * may write. The projection out of an unnamed temporary no longer reaches this at all - see
     * movableTemporary - so what is left here is a place that keeps a name of its own, which is
     * exactly the case where the reader has to choose.
     */
    if(wholeMove(analysis, place)) {
        report(analysis, "this hands ownership on out of a value that still owns it - write `->` on the name that binds it, as in `Just(->v)`, to take it out, or `let &->` where the name must also be writable"_v,
               source);
        return;
    }

    report(analysis, "cannot move a part of a value out of it - move the whole value instead"_v, source);
}

/*
 * Every place one instruction reads, for the use-after-move check over borrowed storage.
 *
 * The first ownership lattice needs nothing like this: `computeEffects` already records a use of
 * the *local* a place is rooted in, and reading a local that was moved out of is exactly what the
 * loop over `effects.uses` reports. A borrow-rooted place has no such local - what its read
 * records is the local the access path starts in, and that local is as initialized as it ever was,
 * so a read of storage this body has emptied looked like a read of a perfectly good environment
 * pointer. This is what asks the second lattice the same question the first one is asked.
 *
 * `eachPlace` with one exception, rather than the ten-arm switch this was: every kind that names a
 * place names it here, so a kind that gains one reaches this check without an edit.
 *
 * The exception is the fill. An unprojected `Init` or `Assign` is what *puts* something in the slot,
 * and filling a slot that was moved out of is the point; a projected one writes one field and leaves
 * the rest, so the rest had better still be there.
 */
template<class F>
static void eachReadPlace(Inst& instruction, F&& visit) {
    if(instruction.kind == Value::Init || instruction.kind == Value::Assign) {
        auto& write = (InstInit&)instruction;
        if(write.place.projections.isEmpty()) return;
    }

    eachPlace(instruction, visit);
}

/*
 * Use after move, and the moves that cannot be represented at all.
 */
void checkMoves(Analysis& analysis) {
    // `at` is the departure's index, which the note beside P3's diagnostic needs to look forward
    // from - see nextUseAfter. Set by the loop below before each instruction's operands are walked.
    Size at = 0;

    auto transfer = [&](ModulePtr<Value> value, LocationId source) {
        checkTransfer(analysis, value, source, at);
    };

    /*
     * Which borrowed storage this body took ownership out of, and where it left.
     *
     * One list over `BorrowedPlace`, so a `&` binding and a captured `&` are entries of the same
     * kind however differently the state behind each is kept. It stays empty for every body that
     * does not empty a borrow, which is all but a handful - see the release-point loop at the end,
     * which is skipped entirely when it is.
     *
     * The first move rather than the last: it is the one whose obligation the later ones inherit,
     * and it is the line the reader has to change.
     */
    struct Emptied {
        BorrowedPlace place;
        LocationId source;
        bool reported = false;
    };

    SmallArray<Emptied, 2> emptied;

    // Which slots have already had a use-after-move reported - see the loop below.
    auto& reportedUse = analysis.scratch.reportedUse;
    reportedUse.reset(analysis.localCount);

    auto recordEmptied = [&](const BorrowedPlace& place, LocationId source) {
        for(auto& seen: emptied) {
            if(seen.place.local == place.local && seen.place.slot == place.slot) return;
        }

        emptied.push(Emptied { place, source });
    };

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        auto& states = analysis.stateBefore[i];
        auto& effects = analysis.effects[i];

        /*
         * A `ret` counts, because returning a value of type `a` is a hand-over like any other.
         *
         * `fn identity(v: a) -> a = v` promises the caller an `a` it may drop, out of storage the
         * caller lent and still owns. The two honest signatures are `->v`, which takes it, and a
         * `return` marker with a borrow result - `fn identity(return v: a) -> &a` - which promises
         * only a view. What could not stay is the third reading, where the same parameter is
         * borrowed on the way in and owned on the way out.
         *
         * Returning a whole *owned* slot never reaches here: returnValue makes it an InstMove, and
         * a move is the thing this check exists to ask for.
         *
         * A phi's inputs are here too, and used to be a loop of their own over every block's phi
         * list. They departed on the edge into the join rather than at the join, which is where
         * attributePhiEdges puts the *transfer* - but that is a statement about where ownership
         * changes hands, not about where the operand is written, and `numberFunction` puts a phi in
         * the instruction order like everything else. Two loops meant the enumeration had to name
         * which points belonged to which, and naming it once is the whole of eachTransferOperand.
         */
        at = i;
        eachTransferOperand(analysis.local, instruction, transfer);

        /*
         * A `->` argument out of borrowed storage, which is the same rule the `Move` arm below
         * imposes on the same storage - only the instruction differs, and only on one path.
         *
         * A concrete call has the move: `sinkValue` puts an `InstMove` in front of every `->`
         * argument that names a place, and the arm below is what sees it. A deferred class dispatch
         * has none - `emitGenericDispatch` applies no conventions, because the callee is not a
         * function that call site reaches and lowering adapts each argument to the erased ABI
         * itself - so `%v = load %target : a` is the whole of the hand-over there. The ownership
         * lattice was already told (computeEffects' GenCall arm reads the same declaration and
         * calls transferFromArgument), and the *check* was not, so the obligation to write
         * something back existed in one build of a body and not the other:
         *
         *     fn (Eat(a)) drain(&s: a) -> {} = eat(s)     -- `eat` takes `->x: a`
         *
         * was rejected specialized and accepted generic. Nothing shipped that way, since the driver
         * always specializes and every `lib/` fixture is built both ways - but a rule that holds in
         * one representation of a body and not in another is not a rule, and which one a call site
         * gets is an optimization decision.
         *
         * Only borrowed storage, deliberately. A `->` argument out of a slot this frame *owns* is
         * the ordinary hand-over, and everything that has to be said about it - a use afterwards, a
         * partial move - is said by the loops below off the effects this instruction already
         * records. What has no other voice is the borrow, whose root stays as initialized as it ever
         * was however much has been taken out of the storage it names.
         *
         * An argument that already is an `InstMove` is left to the arm below, so that a body which
         * writes the hand-over out states one mistake once.
         */
        if(auto arguments = callArguments(instruction)) {
            U16 position = 0;

            for(auto arg: arguments->contents(analysis.local)) {
                auto index = position;
                position++;

                if(!declaredSink(analysis, instruction, index)) continue;
                if(!arg || analysis.local[arg]->kind == Value::Move) continue;

                auto place = argumentPlace(analysis, arg);
                if(!place) continue;

                /*
                 * The obligation only, and deliberately not the refusal the `Move` arm pairs it
                 * with.
                 *
                 * That refusal - "cannot take ownership of borrowed storage" - reports on storage
                 * the *resolver* decided to move out of, and on this path there was no such
                 * decision to report: `sinkValue` picks between a move and a copy by the argument's
                 * ownership, and a deferred dispatch is the one call form where that pick has not
                 * been made. `Hmac.of` in lib/Digest is the case. Its `let &inner = state` is a loan
                 * of a `->` parameter, and `finish(inner)` reads as a hand-over out of a borrow in
                 * the generic body while every instantiation of it emits `copy` - `Blocks(Md5State)`
                 * is TrivialCopy, so nothing is taken out of anything. Reporting here would reject a
                 * function on the strength of a decision its concrete forms all make the other way.
                 *
                 * What is left is the half that holds however the pick goes: storage this body may
                 * have emptied has to hold something again before the frame lets go of the
                 * reference. Where the pick turns out to be a copy the slot was never emptied, the
                 * state stays Owned, and the release-point loop below has nothing to say.
                 */
                auto borrowed = borrowedPlaceOf(analysis, *place);
                if(borrowed.emptiable) recordEmptied(borrowed, instruction.source);
            }
        }

        if(instruction.kind == Value::Move) {
            auto& moved = (InstMove&)instruction;

            // A partial move would leave the slot half-owned, and every later drop of it would
            // have to know which half. That is a drop flag per field and a drop that runs over a
            // subset of members - real work, deferred deliberately rather than approximated.
            if(!wholeMove(analysis, moved.place)) {
                report(analysis, "cannot move a part of a value out of it - move the whole value instead"_v,
                       instruction.source);
                continue;
            }

            /*
             * Taking ownership out of storage this frame does not own, and putting it back.
             *
             * One rule asked of one thing. `borrowedPlaceOf` is where the two shapes borrowed
             * storage has in the IR are reconciled - a `&` binding, which is a slot in this frame
             * naming the caller's, and a captured `&`, which reaches its storage through a borrow
             * value and has no slot at all. The difference is which lattice carries the state and
             * nothing else, so everything below is written once and holds for both.
             *
             * The move is *allowed* here and settled at the release points at the end of this
             * function, where the state says whether anything was written back. Recorded rather
             * than reported, because the line worth pointing at is this one and the fact that
             * decides it is somewhere else.
             */
            auto borrowed = borrowedPlaceOf(analysis, moved.place);

            if(borrowed.borrowed) {
                if(!borrowed.emptiable) {
                    report(analysis, "cannot take ownership of borrowed storage - a borrow never owns what it refers to, so hand the value over with `->` or duplicate it with `copy`"_v,
                           instruction.source);
                    continue;
                }

                recordEmptied(borrowed, instruction.source);

                /*
                 * A second move with nothing put back between them, reported only where the
                 * general machinery cannot reach it.
                 *
                 * `effects.uses` is keyed by local, so any place with a row there is already
                 * covered by the use-after-move loop below and saying it here as well states one
                 * mistake twice. Storage reached through a borrow has no such row - what a move
                 * through one *uses* is the local its access path starts in, and that local is as
                 * initialized as it ever was - so this is the only thing that will say it.
                 */
                if(borrowed.local == maxLimit<U32> &&
                   borrowedStateAt(analysis, i, borrowed) != OwnState::Owned) {
                    report(analysis, "this storage has already been moved out of and nothing has been written back to it"_v,
                           instruction.source);
                }

                /*
                 * The use loop below is skipped for this instruction, and only for this one.
                 *
                 * What a move out of borrowed storage *uses* is the storage it is taking, and the
                 * load that produced the value already said so one instruction earlier - at the
                 * token the reader wrote, where this instruction's source is the binding it goes
                 * into. Letting both speak reports one mistake twice, in the wrong order, and the
                 * second of the two points at the name receiving the value rather than at the one
                 * that has nothing left to give.
                 */
                continue;
            }

            if(moved.place.root == PlaceRoot::Global) {
                report(analysis, "cannot take ownership of a global - its storage outlives every frame that could take it"_v,
                       instruction.source);
                continue;
            }
        }

        /*
         * The same question the loop below asks of a local, asked of storage that has no local to
         * be asked about - see eachReadPlace, and the double-move check above for the same
         * division of labour. Skipped outright for bodies with no borrow slots, which is nearly all
         * of them.
         */
        if(analysis.borrowSlots.isNotEmpty()) {
            eachReadPlace(instruction, [&](const Place& place) {
                auto read = borrowedPlaceOf(analysis, place);
                if(!read.emptiable || read.local != maxLimit<U32>) return;

                auto state = borrowedStateAt(analysis, i, read);
                if(state == OwnState::Owned) return;

                report(analysis,
                       state == OwnState::Moved
                           ? "this storage has been moved out of and nothing has been written back to it"_v
                           : "this storage may have been moved out of on some paths reaching here"_v,
                       instruction.source);
            });
        }

        for(auto use: effects.uses) {
            if(states[use] == OwnState::Owned || states[use] == OwnState::Uninitialized) continue;

            /*
             * Once per slot, however many uses reach it.
             *
             * `uses` is not a list of places the reader wrote - it is what `useValue` walked to,
             * which climbs from a call's argument back through the load that produced it to the
             * slot the load named. For an owned local the two coincide and always did. For one
             * behind a `&` they do not: reading through a borrow is a load and then a use of what
             * it loaded, so `length(acc)` named `acc` three times and reported it three times, at
             * two columns of the same line.
             *
             * The first is the one to keep. A value that is gone is gone, and every later mention
             * of it is the same mistake with a different column - what the reader has to change is
             * where it left.
             */
            if(reportedUse[use]) continue;
            reportedUse.set(use, true);

            auto name = analysis.tracked[use].name;
            auto moved = states[use] == OwnState::Moved;

            if(name) {
                report(analysis,
                       moved ? "%@ has been moved out of and cannot be used again"_v
                             : "%@ may have been moved out of on some paths reaching here"_v,
                       instruction.source, analysis.context.findName(name));
            } else {
                report(analysis,
                       moved ? "this value has been moved out of and cannot be used again"_v
                             : "this value may have been moved out of on some paths reaching here"_v,
                       instruction.source);
            }
        }
    }

    /*
     * The other half of the rule: what was taken out has to be back before the borrow is.
     *
     * `acc = acc ++ part` through a `&` is safe for a reason the compiler can state rather than
     * guess at - the storage is emptied, a value is written back, and only then does the frame let
     * go of the reference. So this is not a relaxation of the ownership model but the model asked
     * one program point later. What made the blanket refusal look necessary was that the point had
     * no state to be asked at: a borrow root has no row in the first lattice, which is what
     * computeBorrowOwnership exists to give it.
     *
     * Every `ret` is a release point and nothing else is. There is no unwinding in this language -
     * a failed check aborts the process rather than running teardowns on the way out - so the
     * window between the move and the write cannot be observed by anything but this body, and this
     * body reading through it is the use-after-move above. A call taking the same borrow is that
     * same use.
     *
     * `Maybe` is refused rather than flagged. A local resolves one with a drop flag, because the
     * flag lives in the frame that owns the storage; here that frame is the caller's, and this one
     * has nowhere to write a bit the caller would have to read. So a body that fills the slot on
     * one path and returns without filling it on another is a body that has to say which.
     */
    if(emptied.isEmpty()) return;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        if(analysis.local[analysis.order[i]]->kind != Value::Ret) continue;

        for(auto& entry: emptied) {
            // One diagnostic per storage however many returns fail to fill it, since they are all
            // the same mistake stated at the same line.
            if(entry.reported) continue;
            if(borrowedStateAt(analysis, i, entry.place) == OwnState::Owned) continue;

            entry.reported = true;

            /*
             * Worded for the reader rather than for the pass. The release point this was proved at
             * is a `ret`, but the body in front of the reader may be a `for` whose lines were
             * lifted into a continuation without their saying so - so naming the return would send
             * them looking at the wrong brace. What is true in both shapes is that some path leaves
             * here without putting anything back.
             *
             * A captured `&` goes unnamed. The local its path is rooted in is the environment the
             * continuation was handed, and "env" names nothing the reader wrote - the name that
             * would help is the field's, which is a step along the path rather than the root.
             */
            auto name = entry.place.local != maxLimit<U32>
                ? analysis.tracked[entry.place.local].name : StringId {};

            if(name) {
                report(analysis, "%@ is moved out of here and not written back on every path out - the storage belongs to somebody else, who would release a value that has already left"_v,
                       entry.source, analysis.context.findName(name));
            } else {
                report(analysis, "this takes ownership out of borrowed storage and does not write a value back on every path out - the storage belongs to somebody else, who would release what has already left"_v,
                       entry.source);
            }
        }
    }
}

/*
 * Whether the frame-extent check already has this function's escape, with a better locus.
 *
 * `checkFrameExtent` names the descriptor's own slot - "this borrow of `xs`", at the expression that
 * built the view - where `checkReturnRoots` can only name the signature. They are two reports of one
 * mistake wherever both apply, so the more specific one wins and this is what the other asks.
 *
 * The condition is `checkFrameExtent`'s own, read from here so the two cannot drift apart: a slot
 * that escaped, carrying a `viewOf` whose root is not a live parameter. A view of a parameter's
 * container is the return-root check's business precisely because this one declines it.
 */
static bool reportedAsFrameView(Analysis& analysis) {
    for(Size l = 0; l < analysis.localCount; l++) {
        if(!analysis.escaped[l]) continue;

        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(slot.materialized) return true;
        if(slot.viewOf == maxLimit<U32>) continue;

        auto viewed = analysis.function.localAt(analysis.local, viewedRoot(analysis, slot.viewOf));
        auto owner = viewed.value && analysis.local[viewed.value]->kind == Value::Arg
            ? (Arg*)analysis.local[viewed.value] : nullptr;

        if(!owner || owner->convention == ast::BindType::Sink) return true;
    }

    return false;
}

/*
 * The return-root check (Design.md's "Borrows in return position").
 *
 * The declaration is the contract and the body is what has to fit it, so this compares two things
 * the summary already holds: the group the signature declared, and the roots resolving every return
 * path actually found. Nothing here looks at a callee's body - a call's result arrived with the
 * callee's declared group already mapped through the operands, which is what makes provenance
 * compose transitively through a helper without inspecting one.
 *
 * A raw pointer result is held to the same contract, which is Analysis-Language.md §1. The instinct
 * that a `%T` is outside the ownership model is right about the pointer's *target* - that is
 * checkTransfer's `outsideModel`, and it stays - and wrong about where the address came from, which
 * this model computes for a pointer exactly as it computes it for a borrow: refersToStorage tests
 * isPointer, so `slots(r)` already arrives here carrying `r`. Leaving it unchecked was the one place
 * the compiler knew the answer and did not say it, and what it cost was `bytesOf(terminated(p))`
 * reading a path out of storage the allocator had already taken back.
 *
 * Asked of a result that *is* a pointer and not of one that merely contains one, which is the same
 * line containsBorrowLike draws and for the same reason: a raw pointer is where this analysis stops
 * by construction, so `Array(T)` holds a `Run(T)` holds a `%T` and descending would make every
 * container-returning function in the library declare a group. A pointer stored into something
 * longer-lived than its root is the separate hole §1 names at its end, and it is not this one.
 */
void checkReturnRoots(Analysis& analysis) {
    auto& function = analysis.function;
    auto& summary = function.summary;
    if(!summary.returnsBorrow && !isPointer(analysis.global, function.returnType)) return;

    auto source = function.source;

    /*
     * A borrow rooted in a local, a global, or a sunk parameter has no caller-side root that could
     * keep it alive, which is a different mistake from being rooted in the wrong argument.
     *
     * Asked of a result that *is* a reference, and of one that owns nothing and merely contains one.
     * The asymmetry is provenance being field-insensitive: a record holding both an owned array and
     * a slice has local roots for the array, and they are not a mistake - the array goes with the
     * result. Reporting on them would reject `data Mixed {view: '[Int], owned: [Int]}` for the half
     * that is doing nothing wrong.
     *
     * **The type answers the half that matters, without provenance becoming field-sensitive.** A
     * local root is ambiguous only where the result could legitimately have one, and it could only
     * where the result *owns* something in this frame - the array half of `Mixed`, whose storage
     * goes with the result and whose root is not a mistake. A result that owns nothing has no such
     * half: every root it has is reached through a reference, so a local one is the mistake with no
     * second reading. `Pair('Int, 'Int)` is exactly that shape and is Analysis-Borrows.md §2.4's own
     * example; `data Holder {bytes: '[U8]}` is the same shape with a window in the field.
     *
     * `needsTeardown` is the question asked, because owning something in this frame and having to
     * release it are the same fact. What is still not reached is a result that owns *and* refers -
     * `Mixed` built here and handed back - and that one needs the path each reference sits at, which
     * is §6.6's.
     *
     * **A window in the field counts, which is why the question asked is `containsBorrowLike`.**
     * `checkFrameExtent` reports a stored window too, and better - it names the container and the
     * expression that took the view - but only where the descriptor's slot carries a `viewOf`, which
     * is written when the conversion happens at the store. A window that arrived from a call has
     * none, so `Holder {bytes: elements(xs)}` reaches no other check and `Holder {bytes: xs}` reaches
     * both. `reportedAsFrameView` is the second half of that: the broad walk finds every shape and
     * the specific report wins wherever it applies, so each mistake is stated once and by whichever
     * check can say the most about it. `test/resolve/Reject.WindowRoot.yana` is the shape that had
     * neither.
     */
    auto ownsNothing = containsBorrowLike(analysis.module, function.returnType) &&
                       !needsTeardown(analysis.module, function.returnType) &&
                       !reportedAsFrameView(analysis);

    if(summary.invalidRoot &&
       (isBorrowLike(analysis.module, function.returnType) || ownsNothing)) {
        report(analysis,
               "a borrow returned from this function is rooted in storage the caller does not own - it must come from an argument, and the argument's type must say which loan it hands back"_v,
               source);
    }

    auto undeclared = summary.actualRoots & ~summary.declaredRoots;
    if(!undeclared) return;

    // One rule, and the noun is the only thing that differs. A reader told a *pointer* is unrooted
    // has been told what to look at; told a "borrow" about a `-> %U8` they would look for a `&`
    // that is not there.
    auto pointerResult = isPointer(analysis.global, function.returnType);

    U16 index = 0;
    for(auto argPointer: function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];

        if(undeclared & rootBit(index)) {
            report(analysis,
                   pointerResult
                       ? "a pointer returned from this function is rooted in %@, which the signature did not mark `return`"_v
                       : "a borrow returned from this function is rooted in %@, which the signature did not put in a loan group - write a leading `'` on that parameter's type, so that the caller is told the result is rooted in it"_v,
                   arg->source, analysis.context.findName(arg->name));
        }

        index++;
    }
}

/*
 * A loan that left the extent its signature gave it.
 *
 * Five things used to ask this, one function each, and every one of them opened by reading
 * `analysis.escaped` and then differed only in which slot it was looking at and what it said. They
 * are one statement - *something borrowed outlived what was keeping it valid* - and the useful part
 * is not the loop but the answer to the second question: **what said this loan's extent**. There
 * are two kinds of answer, and that is the division below rather than five.
 *
 * A *declaration* says it, on behalf of implementations no call site can see. A class function's
 * borrowed parameter and a class iterator's continuation are both of these: a generic body cannot
 * see which instance runs, so it takes the declaration's word, and what makes the declaration true
 * is checking every implementation against it here. See assumedRetained in analyze_escape.cpp,
 * which is the assumption these pay for.
 *
 * Or this frame's *own storage* says it, and then the reason is a property of the slot: a slice is
 * a view of a container this frame is about to tear down, a widened `&` argument is a temporary the
 * return would have narrowed back, a closure environment holds a reference to the frame that built
 * it. Three slot flags, three sentences, one loop.
 *
 * All of this is what Analysis-Borrows.md §6.3 calls "an ad-hoc repair at one dispatch mechanism
 * instead of the meaning of a borrowed parameter", and §8.4 replaces the lot with ordinary
 * signature conformance once a loan is part of a type. Collapsing them first is what makes that one
 * edit rather than five: the reasons stay, the five entry points do not.
 */

// Which promise one parameter is held to, or None where this function made none - which is every
// function that does not implement a class.
enum class DeclaredExtent: U8 {
    None,

    // An ordinary borrowed parameter of a class implementation or a class default.
    Parameter,

    /*
     * The continuation a `for` loop appends. It is the last parameter, always - the desugaring puts
     * it there, so an implementation cannot have written one of its own after it - and it is a
     * separate answer because it is the one a caller could not have declared and the one `->`
     * cannot rescue. See Function::classContinuation and emitGenericDispatch.
     */
    Continuation,
};

static DeclaredExtent declaredParameterExtent(Analysis& analysis, U16 index) {
    auto& function = analysis.function;

    if(function.classContinuation && index + 1 == function.args.size()) {
        return DeclaredExtent::Continuation;
    }

    // A class *default* is held to the rule as well, and has to be: an instance that supplies no
    // implementation puts the default in the slot, so a deferred dispatch reaches it exactly as it
    // reaches a written one. See Function::classDefault.
    if(function.instanceOf || function.classDefault) return DeclaredExtent::Parameter;

    return DeclaredExtent::None;
}

// The parameters a declaration promised would not be retained, and were.
static void checkDeclaredExtents(Analysis& analysis) {
    auto& function = analysis.function;
    if(!function.instanceOf && !function.classDefault && !function.classContinuation) return;

    U16 index = 0;

    for(auto argPointer: function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];
        auto kind = declaredParameterExtent(analysis, index);
        index++;

        // `->` is both the escape hatch and the answer to the diagnostic: a member that has to
        // store what it is given asks for it by value. A continuation has no such hatch, which is
        // why it is a separate sentence rather than a separate check.
        if(kind == DeclaredExtent::None || arg->convention == ast::BindType::Sink) continue;

        auto slot = backingLocal(analysis, (ModulePtr<Value>)argPointer);
        if(slot == maxLimit<U32> || !analysis.escaped[slot]) continue;

        if(kind == DeclaredExtent::Continuation) {
            report(analysis, "this implements a class %@, so its continuation cannot outlive the call - a `for` loop in a generic body cannot see which implementation runs, and takes the declaration's word that the continuation is called rather than stored"_v,
                   arg->source,
                   function.funKind == ast::FunKind::Iter ? "`iter fn`"_v : "`lens fn`"_v);
        } else {
            report(analysis, "this implements a class function, so it cannot keep %@ beyond the call - a call in a generic body cannot see which implementation runs, and takes the declaration's word that a borrowed argument's extent is the call. Declare the parameter `->` if the body has to store what it is given"_v,
                   arg->source, analysis.context.findName(arg->name));
        }

        /*
         * And where it was kept - Analysis-Borrows.md §8.4's "the storing instruction and the
         * parameter declaration".
         *
         * The error above is on the declaration, which is where the promise is and so where the fix
         * usually goes; on its own it leaves a reader to find the write in a body that may have
         * several candidates and whose escape closed over containment. `escapeSite` is the
         * instruction that made the fact true, and a note is the right shape for it: it is the
         * evidence rather than a second thing wrong.
         *
         * Null is a real answer and is why this is conditional - a parameter can be escaped by
         * inheritance through a root something else marked, and pointing at that root's instruction
         * would name a line the reader cannot act on.
         */
        auto site = slot < analysis.escapeSite.size() ? analysis.escapeSite[slot] : nullptr;
        if(site) note(analysis, "it is kept here"_v, analysis.local[site]->source);
    }
}

/*
 * One escaped local, and what this frame was keeping it valid with.
 *
 * The three flags are mutually exclusive by construction - a closure environment is not a view and a
 * view is not a materialized temporary - so the order they are tested in is a formality rather than
 * a precedence, and the early returns say that rather than implying a fallthrough that cannot
 * happen.
 */
static void checkFrameExtent(Analysis& analysis, Size local) {
    auto global = analysis.global;
    auto slot = analysis.function.localAt(analysis.local, U32(local));
    auto source = slot.value ? analysis.local[slot.value]->source : analysis.function.source;

    /*
     * The one borrow that is still a temporary, escaping.
     *
     * A `&` argument whose declared type is wider than the field's - `increment(&x: Int)` given a
     * `@bits(13)` field - is widened into a temporary and narrowed back when the call returns,
     * because a reference cannot convert. That is right for a callee that reads and writes through
     * it and wrong for one that keeps it: the temporary dies with the frame, and nothing would ever
     * narrow.
     *
     * Nothing about packing is left in this check. A borrow of a narrow field at its *own* type is a
     * reference that carries the field's shift, works wherever the field is, and outlives whatever
     * it likes - see NarrowRef in resolve/lower.cpp. What is reported here is a conversion with
     * nowhere to happen, and the fix is in the signature.
     */
    if(slot.materialized) {
        report(analysis, "cannot borrow this beyond the call - the callee declared a wider type than the field has, so what it receives is a temporary written back when the call returns, and this callee keeps it. Declare the parameter at the field's own type"_v,
               source);
        return;
    }

    /*
     * A slice may not outlive the container it is a view of - Implementation-Containers.md §4.
     *
     * The descriptor holds a `%T` into the run, and a raw pointer is outside the ownership model by
     * construction, so nothing about the *fields* of a slice says it refers to anything. What says
     * so is Local::viewOf, written where the descriptor was built out of an owned array, and this is
     * the check that spends it: the slice escaped this frame while the array it points into is this
     * frame's, so the frame is about to run that array's teardown and free the run underneath it.
     *
     * Precise rather than provenance-shaped, deliberately. Asking "does the returned value contain a
     * borrow" would have to go through the containment relation, which is field-insensitive - so a
     * record holding both an owned array and a slice would be rejected for the owned half. This asks
     * about the slice's own slot and nothing else, and a slice's slot is only ever a view when this
     * frame made it one.
     *
     * A slice that came *in* as a parameter has no `viewOf` and is not reported here: what it refers
     * to is the caller's, and whether the caller may hand it on is the caller's own return-root
     * check.
     */
    if(slot.viewOf != maxLimit<U32>) {
        auto viewed = analysis.function.localAt(analysis.local, viewedRoot(analysis, slot.viewOf));

        /*
         * A view of a *parameter's* container is the return-root check's business, not this one's.
         *
         * What this is about is a container this frame owns and is about to tear down; a parameter's
         * is the caller's, outlives the call, and is exactly what
         * `fn elements(return self: Array(a)) -> '[a]` hands a view of -
         * Implementation-Containers.md §5. Whether *this* signature is allowed to hand it back is
         * one question with one answer, and checkReturnRoots is where it is asked: deriveSummary
         * walks the same `viewOf` chain, so a view of an argument the signature did not mark
         * `return` is reported there and naming the argument. A sunk parameter is not exempt - the
         * callee owns what it was given, so its teardown is this frame's like any other local's.
         */
        auto owner = viewed.value && analysis.local[viewed.value]->kind == Value::Arg
            ? (Arg*)analysis.local[viewed.value] : nullptr;

        if(owner && owner->convention != ast::BindType::Sink) return;

        auto viewSource = slot.value ? analysis.local[slot.value]->source : analysis.function.source;

        if(viewed.name) {
            report(analysis, "this borrow of %@ outlives the frame that owns it - a slice is a view into the container's storage, and the container is released when this function returns"_v,
                   viewSource, analysis.context.findName(viewed.name));
        } else {
            report(analysis, "this borrow of an array outlives the frame that owns it - a slice is a view into the container's storage, and the container is released when this function returns"_v,
                   viewSource);
        }

        return;
    }

    /*
     * A closure that outlives the frame cannot hold a borrow of it.
     *
     * Design-Memory §8's third case says a closure that must outlive the frame that built it has to
     * own what it captures, and this is where that is checked: the environment escaped, so anything
     * in it that is a `&T` names storage this frame is about to stop guaranteeing. The capture
     * conventions are chosen before any of this is known - a capture is decided at the name that
     * made it, and whether the closure escapes is a whole-function fact - so the two meet here
     * rather than at the lambda.
     *
     * A closure that is merely *called* does not trip this. Nothing marks the environment escaped at
     * an InstCallDyn, deliberately: a lifted body has no way to name its own environment, so it
     * cannot store one, and treating every call as a handover would reject every closure that is
     * used.
     */
    if(slot.closureEnv && slot.type && global[slot.type]->kind == Type::Tup) {
        for(auto field: ((TupType*)global[slot.type])->fields.contents(global)) {
            /*
             * A captured slice is a captured reference - see isBorrowLike - and it is reported
             * separately because the two have nothing to say to each other. A `&T` capture is about
             * the *convention* the capture was made under, which is what the enclosing binding's
             * mutability decided; a slice is a reference whatever convention it travelled by, since
             * copying the descriptor copies the address inside it.
             */
            if(isBorrow(global, field.type)) {
                report(analysis, "this closure outlives the frame that built it, so it cannot capture %@ by reference - the enclosing binding is %@, and a capture of mutable storage is always by reference (Design-Memory §8)"_v,
                       source, analysis.context.findName(field.name),
                       ((BorrowType*)global[field.type])->mut ? "mutable"_v : "borrowed from somewhere else"_v);
            } else if(isBorrowLike(analysis.module, field.type)) {
                report(analysis, "this closure outlives the frame that built it, so it cannot capture %@ - it is a slice, which is a view into a container someone else owns, and copying the descriptor copies the address inside it (Design-Memory §8)"_v,
                       source, analysis.context.findName(field.name));
            }
        }
    }
}

void checkLoanExtents(Analysis& analysis) {
    for(Size l = 0; l < analysis.localCount; l++) {
        if(analysis.escaped[l]) checkFrameExtent(analysis, l);
    }

    checkDeclaredExtents(analysis);
}

/*
 * A reference placed into storage that never said it would hold one - Analysis-Borrows.md §8.2's
 * body-side half, and §7.4.
 *
 * The declaration side has been built for a while: a signature says which of its arguments a loan
 * may outlive the call through, `settleLoanCandidates` decides whether the result is one of them,
 * and `checkDeclaredExtents` holds an implementation to what its class promised. All of that is
 * about the *boundary*. Nothing asked what a body does in between, and the two facts a reference
 * type carries - that the storage is somebody else's, and which of the signature's relationships
 * it is in - were therefore checkable only where the value crossed back out through a result.
 *
 * There are two other ways out, and both were open:
 *
 *   - into a **global**, which §7.4 says may hold an owned value or program-lifetime storage and
 *     nothing else. `gLoose.held = localWindow` compiled, and what the global then named was a
 *     frame that had returned.
 *   - into **storage the caller passed in**, which is §4.7's contract. That one has a spelling -
 *     `remember(&store: Store(kept'), value: kept'Cell)` - and the caller is held to it by
 *     `checkLoanExtents`. What was missing is the other end: that the body may put in `store.held`
 *     only a reference that really is in group `kept`, rather than a frame local or an argument in
 *     no group at all.
 *
 * One rule covers both, and it is the destination that states it: **the storage a reference is
 * placed into decides how long that reference must be good for.** This frame's own storage asks for
 * nothing - `checkBorrows` and `checkLoanExtents` already bound it. A global asks for
 * program lifetime. A destination reached through an argument asks for that argument's group,
 * which the type at the end of the path has to publish and the incoming value has to be in.
 *
 * Field-insensitivity is what keeps this from being stated once and for all: provenance says a
 * value may name argument 2 and local 5, not which member of it does. So the rule is asked of the
 * *store*, where the destination is one place and the source is one value, and the return keeps its
 * own check.
 */

// What one destination demands of a reference written into it.
enum class LoanDestination: U8 {
    // This frame's own storage, which nothing here has an opinion about.
    Frame,

    // A global, which per §7.4 takes program-lifetime storage and owned values only.
    Global,

    // Storage reached through an argument, which takes that argument's group.
    Argument,

    // A raw pointer's target, which is outside the model by construction - see §2.7.
    Untracked,
};

struct LoanDemand {
    LoanDestination kind = LoanDestination::Frame;

    // The argument the path is rooted in, for LoanDestination::Argument.
    U16 argument = 0;
    LoanGroup group = kNoLoan;

    // Whether the type at the end of the path publishes a slot - a field written `kept'Cell`. A
    // destination that does not is one no caller could have been told about.
    bool slot = false;
};

// The argument a local backs, or null for an ordinary frame slot. A parameter with a slot of its
// own is what a `&` argument and a memory-typed one both have, and it is the only way a store's
// destination can be storage this frame does not own.
static Arg* argumentOfLocal(Analysis& analysis, U32 slot) {
    for(auto argPointer: analysis.function.args.contents(analysis.local)) {
        if(backingLocal(analysis, (ModulePtr<Value>)argPointer) == slot) {
            return analysis.local[argPointer];
        }
    }

    return nullptr;
}

// The loan group a type publishes, whichever of the two shapes a reference has - see §4.5 for why
// a slice is the second one rather than a special case of the first.
static LoanGroup publishedGroup(Analysis& analysis, TypePtr type) {
    if(!type) return kNoLoan;
    if(analysis.global[type]->kind == Type::Borrow) return ((BorrowType*)analysis.global[type])->loan;

    return sliceLoanGroup(analysis.global, type);
}

static LoanDemand demandOf(Analysis& analysis, const Place& place) {
    LoanDemand demand;

    switch(place.root) {
        case PlaceRoot::Global:
            demand.kind = LoanDestination::Global;
            return demand;

        case PlaceRoot::Pointer:
            demand.kind = LoanDestination::Untracked;
            return demand;

        case PlaceRoot::Borrow:
            /*
             * A borrow root is the storage it names, so the demand is that storage's. Resolved by
             * walking back to the place the borrow was taken of, which is the same chain
             * `borrowedSlot` follows and is how a `&` argument reaches its own slot.
             */
            if(auto borrow = analysis.local[place.pointer]) {
                if(borrow->kind == Value::Borrow) {
                    return demandOf(analysis, ((InstBorrow&)*borrow).place);
                }
            }

            demand.kind = LoanDestination::Untracked;
            return demand;

        case PlaceRoot::Local:
            break;
    }

    auto argument = argumentOfLocal(analysis, place.local);
    if(!argument) return demand;

    demand.kind = LoanDestination::Argument;
    demand.argument = argument->index;
    demand.group = argument->loan;

    // What the path arrives at, which is where the slot is written. `Store {held: kept'Cell}` puts
    // it on the field, so the type of `store.held` is what publishes it - not `Store` itself, which
    // is exactly the type `Store` and deliberately carries nothing (§8.2).
    walkPlace(analysis.module, analysis.function, place, [&](const PlaceStep& step) {
        if(!step.broken) demand.slot = publishedGroup(analysis, step.type) != kNoLoan;
        return !step.broken;
    });

    return demand;
}

void checkLoanDestinations(Analysis& analysis) {
    /*
     * Only where nothing more specific has already been said about this body.
     *
     * A class implementation that stores a borrowed parameter into a global breaks two rules at
     * once - it keeps what it was lent, which is what its class promised it would not, and it puts
     * a loan somewhere a loan cannot go - and the first of those is the one worth printing: it
     * names the parameter, names the declaration and says what `->` would do about it. This one
     * would restate the same store in the more general terms. Same reasoning as the rewrite guard
     * in analyze.cpp: a second round of diagnostics about the first round's mistake is noise.
     */
    if(!analysis.ok) return;

    auto& function = analysis.function;

    for(Size at = 0; at < analysis.instructionCount; at++) {
        auto pointer = analysis.order[at];
        auto& instruction = *analysis.local[pointer];
        if(instruction.kind != Value::Init && instruction.kind != Value::Assign) continue;

        auto& init = (InstInit&)instruction;
        if(!init.value) continue;

        // Only a value that can name storage at all, which keeps every arithmetic store and every
        // owned handover out of this entirely.
        auto type = analysis.local[init.value]->type;
        if(!containsBorrowLike(analysis.module, type) && !isBorrow(analysis.global, type)) continue;

        auto demand = demandOf(analysis, init.place);
        if(demand.kind == LoanDestination::Frame || demand.kind == LoanDestination::Untracked) {
            continue;
        }

        ScratchProvenance held(analysis);
        transferredProvenance(analysis, init.value, *held);

        /*
         * A frame root, and *not* a descriptor's own slot.
         *
         * computeProvenance deliberately adds the slot a stored reference sat in to its owner's
         * contents - see storedReference there, which is what carries `Local::viewOf` to
         * checkEscapingViews. For this question that edge is noise: what a window *names* is
         * already in the set through its own contents, and the slot it was built in is this frame's
         * by construction. Reading it as a frame root reported every program that ever put a
         * program-lifetime window in a global, `Loose {held: gArr}` included.
         *
         * An argument arrives in a slot of its own too, and that slot is not this frame's storage
         * either - it is the caller's, which is a different demand and is answered below.
         */
        auto fromFrame = false;
        auto fromCaller = held->args != 0;

        held->locals.forEach([&](Size slot) {
            if(argumentOfLocal(analysis, (U32)slot)) { fromCaller = true; return; }

            auto type = analysis.function.localAt(analysis.local, (U32)slot).type;
            if(type && isBorrowLike(analysis.module, type)) return;

            fromFrame = true;
        });
        if(demand.kind == LoanDestination::Global) {
            /*
             * §7.4: a global holds an owned value, or a reference to storage that lives as long as
             * the program, and nothing else.
             *
             * Only the caller's half is reported, and the asymmetry is measured rather than
             * cautious. A reference to *this frame's* storage written into a global is already
             * answered: analyze_escape.cpp reads the write, finds the destination's roots outlive
             * the frame, and marks what was written escaping - so the storage is heap-allocated and
             * never released, which is what `let &sink = Holder {view: [0], at: 0}` needs and is
             * exactly "a root classified as program-lifetime". The cost of that answer is a leak
             * rather than a dangle, and the leak is §8.9's question, not this one's.
             *
             * A reference the **caller** lent has no such answer. The storage is not this frame's
             * to promote, the loan ends when the call does, and nothing else asks - so this store
             * compiled, and what the global then named was a frame that had returned.
             */
            if(fromCaller) {
                report(analysis,
                       "a global cannot hold a reference the caller lent to this call - the loan ends when the call does, and the global does not. Give the global the value itself, or a reference to storage that lives as long as the program"_v,
                       instruction.source);
            }

            continue;
        }

        /*
         * A destination the caller keeps, filled with something this frame owns.
         *
         * Reported even though escape promotion would keep the storage alive, because what is wrong
         * here is not the lifetime but the contract: `remember(&store: Store(kept'), value:
         * kept'Cell)` tells the caller that what `store` ends up holding is a loan of `value`, and
         * the caller's own check - checkLoanExtents - is stated in exactly those terms. A body that
         * puts something else in is not unsafe, it is untrue, and a contract nothing holds the body
         * to is a contract the caller cannot rely on.
         */
        if(fromFrame) {
            report(analysis,
                   "this installs a reference to storage this frame owns into a destination the caller keeps - what the signature promised the caller would find there is a loan of one of its own arguments"_v,
                   instruction.source);
            continue;
        }

        if(!held->args) continue;

        /*
         * The group, which is the half a `return` marker cannot state: what outlives the call is
         * not the result but what was put in the destination, so what has to agree is the
         * destination's group and the source's - see §4.7.
         *
         * A destination with no slot is the case §8.2 said was refused by the return-root check
         * instead of by a message about the slot. It has one now.
         */
        for(auto argPointer: function.args.contents(analysis.local)) {
            auto arg = analysis.local[argPointer];
            if(!(held->args & rootBit(arg->index))) continue;
            if(arg->index == demand.argument) continue;

            if(!demand.slot || demand.group == kNoLoan) {
                report(analysis,
                       "%@ is a reference the caller lent to this call, and this stores it in a destination whose type does not say it holds one - write the field's own loan group on it, so that the caller knows what it is keeping"_v,
                       instruction.source, analysis.context.findName(arg->name));
            } else if(arg->loan != demand.group) {
                report(analysis,
                       "%@ is not in the loan group this destination holds - the two have to name one relationship for the caller to be checked against it"_v,
                       instruction.source, analysis.context.findName(arg->name));
            }
        }
    }
}
