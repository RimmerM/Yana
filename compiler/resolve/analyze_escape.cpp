#include "analyze_pass.h"
#include "edit.h"

/*
 * What has to outlive the frame, and where that puts it.
 *
 * One file because the second question has no content of its own: an allocation goes on the heap
 * exactly when this pass proved its storage outlives the frame, and everything else about the
 * decision - who releases it, whether a `@heap` binding overrode it - is about *which* of the two
 * escapes it was. See selectStorage at the end.
 *
 * Seeded from the four instructions that can hand storage to something running after this function
 * returns, then closed over containment: if a root outlives the frame, so must everything reachable
 * through it, or the array survives and its buffer does not.
 */

// Which of the two things an escape says about who owns the storage afterwards - see
// Analysis::transferred. `Owned` is the answer whenever this pass can point at the new owner.
enum class Escape: U8 {
    Owned,
    Referenced,
};

static bool markEscaped(Analysis& analysis, const Provenance& roots, Escape kind) {
    auto changed = false;

    // Over what the provenance holds rather than over the frame's locals - see IndexSet::forEach.
    roots.locals.forEach([&](Size l) {
        if(analysis.escaped.add(l)) {
            analysis.outlives.set(l, true);
            changed = true;
        }

        // One root can be both, and being owned elsewhere is the stronger statement: a value handed
        // over is handed over however many other references to it were kept.
        if(kind == Escape::Owned && analysis.transferred.add(l)) changed = true;
    });

    return changed;
}

/*
 * What handing one argument to a call says about the storage behind it.
 *
 * The two cases are the two shapes an argument has, which is why this needs neither a summary nor a
 * signature to decide. An aggregate is passed as the address of storage the caller keeps, so what
 * can outlive the call is what it *contained* - and whatever it contained belongs to the aggregate,
 * whose own teardown is what releases it. A borrow or a pointer is the address itself, so what may
 * outlive the call is a reference to storage that is still this frame's.
 *
 * Which is exactly the distinction Analysis::transferred exists for, and the reason a root handed to
 * a call the pass could not summarize is not thereby leaked.
 */
static Escape argumentEscape(Analysis& analysis, ModulePtr<Value> arg) {
    // The same test transferredProvenance splits on, and necessarily so: this says who owns what
    // that function decided was leaving, so the two have to be reading the argument the same way.
    return isMemoryType(analysis.global, analysis.local[arg]->type) ? Escape::Owned : Escape::Referenced;
}

/*
 * What one *argument* hands over.
 *
 * transferredProvenance answers what a value contributes, and for an aggregate that is what it
 * contained rather than the slot it sat in - the aggregate is copied, so the slot stays behind. A
 * slice is a borrow whose representation is a record (isBorrowLike), and copying it copies the
 * address inside it, so a callee that retains one retains a view of this frame's container. The
 * descriptor's own slot is what carries Local::viewOf, so it has to be in the set or
 * checkEscapingViews never sees the view leave.
 *
 * Arguments only. A returned descriptor is bounded by the return-root check instead, and marking its
 * slot here would put every subslice's storage on the heap for nothing.
 */
static void handedOver(Analysis& analysis, ModulePtr<Value> arg, Provenance& into) {
    transferredProvenance(analysis, arg, into);

    if(arg && isBorrowLike(analysis.module, analysis.local[arg]->type)) {
        joinProvenance(into, provenanceOf(analysis, arg));
    }
}

/*
 * What a callee this pass cannot see through is assumed to do with one argument.
 *
 * It keeps what it was handed by `->` and nothing else - which is a reading of the declaration
 * rather than a guess about the body, and the only one available: a callee with no body is an
 * intrinsic, or a class signature whose implementation is chosen per instance. Both are declarations
 * and nothing else, so the conventions written on them are the whole of the contract.
 *
 * **A borrow's extent is the call.** That is the language's rule everywhere else, and a body is what
 * it takes to contradict it - `retained` is derived from one wherever there is one to derive it
 * from. Reading a borrow as a retention *because* there is no body assumed the opposite of what the
 * declaration says, and it was expensive in the erased form, where the callee every container
 * reaches with no summary is *pointer arithmetic*: `Flat {items: self.items + from}` in
 * `unclampedSlice` is a call to an intrinsic `+`, so `self` came back retained, and that answer
 * climbed through `elements`, `chunks`, `valueAt`, `push` and `insert` until every accessor in the
 * library reported that it kept its container. A specialized build turns the same call into an
 * instruction and none of it happens, which is why it was only ever visible with specialization
 * declined - and why a generic body could not hand a class function a view of anything it owned.
 *
 * A class signature is *held* to this rather than trusted for it: checkClassBorrows reports an
 * implementation that keeps a parameter the signature declared as a borrow, so what is assumed here
 * is true of every instance rather than of the ones some call site happened to look at. It is the
 * same division checkContinuationExtent already draws for a class iterator's continuation.
 *
 * An intrinsic has no such check and needs none - it is an instruction - but it does have to say so
 * where it hands something over. `store(to: %a, ->value: a)` is the one that does, and the `->` is
 * not decoration: the value is written into the pointee, so the frame that owned it has to stop
 * owing it a drop.
 */
static bool assumedRetained(Analysis& analysis, ModulePtr<Function> callee, U16 index) {
    if(!callee) return true;

    auto& declared = analysis.local[callee]->args;
    if(index >= declared.size()) return true;

    return analysis.local[declared.get(analysis.local, index)]->convention == ast::BindType::Sink;
}

// One round of seeds. Separate from the closure below only so that both can be repeated together:
// a store into a root that a later instruction turns out to hand away is an escape too, and one
// pass in instruction order would miss it.
static bool escapeRound(Analysis& analysis) {
    auto changed = false;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];

        switch(instruction.kind) {
            case Value::Ret: {
                auto value = ((InstRet&)instruction).value;
                if(!value) break;

                /*
                 * An aggregate result is copied into storage the caller passed in, so the slot it
                 * came out of stays behind and only what it *contained* leaves. A borrow or a
                 * pointer is the address itself, so the root leaves with it.
                 *
                 * A root that is a *parameter's* slot is deliberately not marked. Handing a borrow
                 * of an argument back is not an escape, it is the return-root mechanism, and the
                 * caller already bounds that storage - the summary says so through its declared
                 * group rather than through this bit. Marking it here would make every accessor's
                 * argument look like something that had to outlive its caller, and every value
                 * anyone ever borrowed from would land on the heap.
                 */
                ScratchProvenance leaving(analysis);
                transferredProvenance(analysis, value, *leaving);

                /*
                 * A slice's descriptor goes with it, and for the same reason it is reached through
                 * Local::viewOf - Implementation-Containers.md §5's `elements`, whose whole body
                 * builds a descriptor out of an argument and hands it back. The descriptor is a
                 * local of this frame, so without this it is a root that is not a parameter's and
                 * lands on the heap - two words allocated and never freed, per call, for storage
                 * whose two words are copied out at the return anyway.
                 */
                for(Size l = 0; l < analysis.localCount; l++) {
                    if(!isParameterSlot(analysis, viewedRoot(analysis, U32(l)))) continue;

                    leaving->locals.set(l, false);
                }

                changed = markEscaped(analysis, *leaving, Escape::Owned) || changed;
                break;
            }

            case Value::Init:
            case Value::Assign: {
                auto& write = (InstInit&)instruction;
                ScratchProvenance roots(analysis);
                placeProvenance(analysis, write.place, *roots);

                auto escaping = roots->global || roots->unknown;
                for(Size l = 0; l < analysis.localCount && !escaping; l++) {
                    escaping = roots->locals[l] && analysis.outlives[l];
                }

                // Owned rather than merely referenced: what was written is reachable through the
                // root it was written into, and that root's teardown is what releases it.
                if(escaping) {
                    ScratchProvenance written(analysis);
                    transferredProvenance(analysis, write.value, *written);

                    /*
                     * A root does not escape into storage it already roots.
                     *
                     * `self.items = hostGrow(self.items, wide)` in `growJsArray` is the shape, and
                     * every part of it is ordinary: the destination is rooted in `self`, the value
                     * is rooted in `self` because an erased `hostGrow` is a GenCall and a GenCall's
                     * result conservatively carries all of its arguments, and `self` is a parameter
                     * so it outlives this frame. Three correct answers compose into the wrong one -
                     * that a container escapes by being grown.
                     *
                     * Nothing new becomes reachable in that write. Escape asks whether storage is
                     * reachable through a root that outlives the frame, and a root reached through
                     * *itself* was as reachable before the write as after; the destination's own
                     * roots are therefore removed rather than the write skipped, so a write that
                     * mixes roots still reports the ones that are genuinely arriving. `p.x = q`
                     * keeps `q`, `p.next = p` keeps nothing, and `self.items = other.items` keeps
                     * `other`.
                     *
                     * This is the escape half of the observation Analysis-Borrows.md's reinit case
                     * makes about the borrow checker: emptying storage through a `&` and filling it
                     * again is one value's lifetime, not two overlapping ones.
                     *
                     * Worth knowing what it was costing, since the seed was one library function
                     * and the damage was not: `growJsArray` reported that it kept `self`, `reserve`
                     * inherited that through its call to it, and `push` inherited it from `reserve`
                     * - so the answer climbed the whole container API from one write, exactly the
                     * way assumedRetained above describes the same climb from a different seed.
                     */
                    roots->locals.forEach([&](Size l) { written->locals.set(l, false); });

                    changed = markEscaped(analysis, *written, Escape::Owned) || changed;
                }

                break;
            }

            /*
             * Every element, written into one run - the same question the case above asks, asked
             * once per element against one place.
             *
             * The run is what decides it: a literal whose buffer outlives the frame is exactly the
             * `[7, 8, 9]` that `Array.escaping` returns, and each element written into it is owned
             * by whatever the buffer is owned by. Missing this left the elements unmarked and the
             * literal placed on the frame it leaves.
             */
            case Value::Aggregate: {
                auto& aggregate = (InstAggregate&)instruction;
                ScratchProvenance roots(analysis);
                placeProvenance(analysis, aggregate.place, *roots);

                auto escaping = roots->global || roots->unknown;
                for(Size l = 0; l < analysis.localCount && !escaping; l++) {
                    escaping = roots->locals[l] && analysis.outlives[l];
                }

                if(escaping) {
                    eachAggregateComponent(analysis.local, aggregate,
                                           [&](const AggregateComponent& component, Size) {
                        ScratchProvenance written(analysis);
                        transferredProvenance(analysis, component.value, *written);
                        changed = markEscaped(analysis, *written, Escape::Owned) || changed;
                    });
                }

                break;
            }

            case Value::Call: {
                auto& call = (InstCall&)instruction;
                auto summary = summaryOf(analysis, call.callee);
                U16 index = 0;

                for(auto arg: call.args.contents(analysis.local)) {
                    auto known = summary && index < summary->args.size();
                    auto retained = known ? summary->args.get(analysis.local, index).retained
                                          : assumedRetained(analysis, call.callee, index);

                    if(retained) {
                        ScratchProvenance leaving(analysis);
                        handedOver(analysis, arg, *leaving);
                        changed = markEscaped(analysis, *leaving, argumentEscape(analysis, arg)) || changed;
                    }

                    index++;
                }

                break;
            }

            case Value::CallDyn: {
                /*
                 * The same question a direct call asks, asked of the *type* instead of a body.
                 *
                 * There is no callee to have a summary, and for a long time that was read as "so
                 * assume everything is kept". It is the wrong reading, and assumedRetained above
                 * already says why for the other callee with no body: a declaration is a contract
                 * and not an absence of one. A borrow's extent is the call everywhere else in the
                 * language, so a body is what it takes to contradict it - and a function value has
                 * no body to do the contradicting.
                 *
                 * What states the contract here is FunType, which interns each argument's
                 * convention and `return` marker precisely so that a caller holding nothing but the
                 * type can read them - see emitDynamicCall, which converts every argument by the
                 * same declarations. So this is assumedRetained's rule over a FunArg rather than
                 * over an Arg, and the two now answer alike for every callee whose body is out of
                 * reach. Analysis-Borrows.md §5.1 and §8.4.
                 *
                 * Retention is the last fact that was decided by which *call form* was written
                 * rather than by what was declared, which is what §6.7's test is for: replacing a
                 * named call with an equivalent function value no longer changes which programs are
                 * legal. What it still changes is how well they compile, which is where a
                 * whole-program summary belongs.
                 *
                 * Measured on the way in, since both were worth knowing:
                 *
                 *  - It is what lets a callback take a borrow at all. Every one of the six
                 *    functions in lib/Core/Sort.yana reported that it kept `xs`, for no reason but
                 *    that the comparison reaches it through a function value; so did every lambda
                 *    over a borrowed local. Twelve sites in lib/ and test/, none of them real.
                 *  - The storage follows. A local handed to a function value was heap-placed
                 *    because it was believed to outlive the call, so `Lambda.yana` allocated a
                 *    counter it immediately freed; it is an alloca now.
                 *
                 * A signature that is absent or is not a function type has no contract to read - a
                 * teardown the compiler calls through a descriptor is the case - and the
                 * conservative answer stands there, exactly as it does in dynamicResultProvenance.
                 * Costs nothing measurable: making that fallback permissive too moved no site.
                 *
                 * One call form is still not covered by any of this, and the block below says which.
                 */
                auto& call = (InstCallDyn&)instruction;

                /*
                 * Except a `yield`, which the contract above cannot state.
                 *
                 * What exempts it is the continuation's *extent* rather than its convention, and
                 * the two came apart the moment the convention started being read: a `-> ->T`
                 * iterator declares its continuation parameter `Sink`, so the rule above answers
                 * "retained" for it and is right about the ownership and wrong about the lifetime.
                 * The value really is handed over, and the continuation it is handed to runs
                 * strictly inside this call - Implementation-Lens.md's bounded-continuation
                 * contract - so nothing leaves the frame and there is nothing to place elsewhere.
                 *
                 * Deleting this in favour of the signature (it looked redundant, since an ordinary
                 * `yield`'s continuation is declared with the default convention) moved
                 * `IterHandover.yana` from an alloca to allocateHeap and pulled the whole heap
                 * allocator into the fixture. That is the measurement that says the flag is a fact
                 * of its own; see InstCallDyn::handover for the one it was added for.
                 */
                if(call.handover) break;

                auto signature = call.signature && analysis.global[call.signature]->kind == Type::Fun
                    ? (FunType*)analysis.global[call.signature] : nullptr;

                U16 index = 0;
                for(auto arg: call.args.contents(analysis.local)) {
                    auto retained = true;
                    if(signature && index < signature->args.size()) {
                        retained = signature->args.get(analysis.global, index).convention
                                       == ast::BindType::Sink;
                    }

                    index++;
                    if(!retained) continue;

                    ScratchProvenance leaving(analysis);
                    handedOver(analysis, arg, *leaving);
                    changed = markEscaped(analysis, *leaving, argumentEscape(analysis, arg)) || changed;
                }

                break;
            }

            case Value::GenCall: {
                /*
                 * An erased call is a different representation of the same call, so the callee's
                 * summary means exactly what it means at a direct one: which parameters are
                 * retained is a property of the body, and substituting types does not change it.
                 *
                 * There is a summary for a call to a generic *function*, whose body was resolved
                 * before the call was deferred. A deferred *class* dispatch names the class
                 * signature, which has no body - summaryOf answers null for it, and what stands in
                 * for one is what its declaration says about each parameter; see assumedRetained.
                 *
                 * Consulting it is what lets an adaptor be written. `for x in upTo(n)` inside an
                 * `iter fn` is a call to a function generic in what the loop body returns, so it is
                 * this instruction rather than a Call - and assuming the continuation was kept made
                 * the lifted body's environment escape, which then rejected its capture of the
                 * enclosing continuation for outliving the frame.
                 */
                auto& call = (InstGenCall&)instruction;
                auto summary = summaryOf(analysis, call.callee);
                U16 index = 0;

                /*
                 * How many positions the callee declares. Every position is one for an erased call;
                 * a deferred class dispatch has one more, because a `for` loop over a class `iter
                 * fn` appends its continuation to a signature that could not declare one - see
                 * emitGenericDispatch.
                 *
                 * That trailing position is *not* retained, and the promise is the declaration's
                 * rather than this call's guess: checkContinuationExtent holds every implementation
                 * of a class iterator to it, which is what makes it safe to assume where there is no
                 * body to consult.
                 */
                auto declared = call.callee ? analysis.local[call.callee]->args.size() : 0;

                for(auto arg: call.args.contents(analysis.local)) {
                    auto known = summary && index < summary->args.size();
                    auto retained = index < declared &&
                                    (known ? summary->args.get(analysis.local, index).retained
                                           : assumedRetained(analysis, call.callee, index));

                    if(retained) {
                        ScratchProvenance leaving(analysis);
                        handedOver(analysis, arg, *leaving);
                        changed = markEscaped(analysis, *leaving, argumentEscape(analysis, arg)) || changed;
                    }

                    index++;
                }

                break;
            }

            default:
                break;
        }
    }

    // Containment closure. A root that outlives the frame drags everything written into it along,
    // and that relation is what connects an array's own storage to its buffer's.
    for(Size l = 0; l < analysis.localCount; l++) {
        if(!analysis.outlives[l]) continue;

        // Over what this root contains rather than over every local the frame has, which is what
        // makes the closure quadratic in the *containment* rather than in the frame - see
        // IndexSet::forEach. The set being walked is not one the body writes: what is written is
        // `escaped`, `outlives` and `transferred`, and the outer loop's own fixpoint is what carries
        // a root reached here back around.
        analysis.contents[l].locals.forEach([&](Size m) {
            // A root contains itself - that is how a parameter's contents are rooted in the
            // parameter - and that says nothing about escaping.
            if(m == l) return;

            // Owned, always: being reachable through a root that outlives the frame is being
            // part of what that root's teardown releases.
            if(!(analysis.escaped[m] && analysis.transferred[m])) {
                analysis.escaped.set(m, true);
                analysis.outlives.set(m, true);
                analysis.transferred.set(m, true);
                changed = true;
            }
        });
    }

    return changed;
}

void computeOutliving(Analysis& analysis) {
    analysis.outlives.reset(analysis.localCount);
    analysis.escaped.reset(analysis.localCount);
    analysis.transferred.reset(analysis.localCount);

    // A parameter's storage is the caller's and already outlives this frame. It is set in
    // `outlives` and not in `escaped`, because nothing here proved anything about it.
    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(slot.value && analysis.local[slot.value]->kind == Value::Arg) analysis.outlives.set(l, true);
    }

    for(Size round = 0; round <= analysis.localCount + 1; round++) {
        if(!escapeRound(analysis)) break;
    }
}

/*
 * Storage-class selection (Implementation-IR.md part 5, Implementation-Regions.md part 4).
 *
 * Cheapest first, which with regions deliberately left out of this milestone is two options: the
 * frame, unless this pass proved the storage has to outlive it. `mayResize` is *not* one of the
 * reasons - an owner whose buffer may be replaced starts on the frame and migrates when it actually
 * grows, which is the whole point of tracking the demand rather than assuming it.
 *
 * Only an allocation has a storage class to choose. A call result or a copy occupies storage the
 * instruction that produced it creates, and if one of those escapes it escapes as a raw pointer,
 * which the language already says nothing about - see the note at the end of analyze.cpp.
 */
/*
 * One allocation, moved from the heap to the frame - see Analysis::demoteOnly.
 *
 * Three things say "heap" about an allocation and all three have to stop saying it together, which
 * is why this is one function rather than three lines at the decision:
 *
 *  - the **instruction**, which is what `lower_mem.cpp` reads to emit `allocateHeap` or an `alloca`;
 *  - the **flag the program reads at run time**, where something asked for one. A run carries a bit
 *    saying whether the allocator owns its storage, and `releaseRun` tests it - so a frame-placed
 *    run whose bit still says heap is a frame address handed to `freeHeap`. See InstAlloc::storageFlag;
 *  - the **drop this frame owes**, where the frame was the owner. `releaseStorage` is the ownership
 *    stage's answer to "who hands this back", and the frame returning is what hands back a frame
 *    slot, so the flag comes off and a drop left with nothing else to do goes with it.
 *
 * The third is the half that only exists here. Before this pass the drop is still an `InstDrop` with
 * a flag on it; after `dischargeOwnership` it is an emitted call to `freeHeap` that would have to be
 * found and deleted instead. That ordering is why the re-run sits where it does, and it is what lets
 * an allocation this frame *releases* be demoted at all rather than only one it handed over.
 */
static void demoteToStack(Analysis& analysis, InstAlloc& allocation, Local& slot) {
    auto& module = analysis.module;

    allocation.storage = StorageClass::Stack;
    allocation.releasedHere = !analysis.transferred[allocation.local] && !allocation.storageFlag &&
                              !allocation.ownedElsewhere;

    if(allocation.storageFlag && analysis.local[allocation.storageFlag]->kind == Value::ConstInt) {
        ((ConstInt*)analysis.local[allocation.storageFlag])->value = 0;
    }

    // The drops that were handing this storage back. Rooted in the local rather than merely naming
    // it, because that is what `insertDrops` built - one drop per local per departure point - and a
    // projection of it is a member's teardown, which is not this storage's release.
    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        if(instruction.kind != Value::Drop) continue;

        auto& drop = (InstDrop&)instruction;
        if(!drop.releaseStorage) continue;
        if(drop.place.root != PlaceRoot::Local || drop.place.local != allocation.local) continue;
        if(drop.place.projections.size() != 0) continue;

        drop.releaseStorage = false;

        /*
         * And the drop itself where handing the storage back was all it did.
         *
         * Left in place it would be the one shape `dischargeDrop` declines - it returns for an empty
         * drop rather than expanding it - so the instruction would survive into a stage whose whole
         * postcondition is that a non-generic body holds none. `insertDrops` never builds one
         * either, which makes an empty drop a thing no other path produces.
         */
        if(drop.isEmpty()) {
            IrEditor(module, analysis.function)
                .eraseInstruction((ModulePtr<Inst>)(&instruction - analysis.local));
        }
    }

    slot.storage = StorageClass::Stack;
    analysis.function.locals.set(analysis.local, allocation.local, slot);
}

void selectStorage(Analysis& analysis, OwnershipResult& result) {
    analysis.releasesStorage.reset(analysis.localCount);

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        if(instruction.kind != Value::Alloc) continue;

        auto& allocation = (InstAlloc&)instruction;
        if(allocation.local >= analysis.localCount) continue;

        auto escapes = analysis.escaped[allocation.local] != 0;
        auto storage = escapes ? StorageClass::Heap : StorageClass::Stack;

        // `@heap` on the binding overrides the analysis in the one direction that is always safe:
        // Design.md's "for a large allocation that's freed well before the region closes".
        auto slot = analysis.function.localAt(analysis.local, allocation.local);
        if(slot.storage == StorageClass::Heap) storage = StorageClass::Heap;

        // The target of a box, which is out of line whatever this pass proved. What frees it is the
        // owner's derived `Reclaim`, which is interned per type and so has one answer for every
        // value of that type - and that answer has to be right for the value that outlived a frame.
        // See InstAlloc::ownedElsewhere.
        if(allocation.ownedElsewhere) storage = StorageClass::Heap;

        /*
         * A run whose length this pass cannot read is on the heap whatever it proved.
         *
         * The frame answer for one is a dynamic alloca, and a dynamic alloca is not released until
         * the frame returns - so a run allocated in a loop would add to the frame once per
         * iteration. Implementation-Containers.md §12's third strategy is exactly that allocation
         * with the placement rule that makes it safe ("selected only when the site is not inside a
         * loop"), and it is deferred; until it lands the conservative answer is the one that cannot
         * overflow a stack. A literal's run has a constant length and is unaffected.
         */
        if(allocation.extent && analysis.local[allocation.extent]->kind != Value::ConstInt) {
            storage = StorageClass::Heap;
        }

        /*
         * The re-run's one rule, and it is here rather than earlier so that it is downstream of
         * every reason above - see Analysis::demoteOnly.
         *
         * Placing it after the overrides is the whole of what keeps them: `@heap` asked for the
         * heap, a box is out of line whatever anything proved, and a run whose length is not a
         * constant has no frame answer at all. Each of those forces `storage` back to Heap before
         * this reads it, so none of them can be undone by an escape that stopped existing.
         *
         * A **closure environment** is left out, and not because the decision would be wrong: the
         * release is the function value's rather than this frame's, and which release it gets was
         * written into the lambda's closure header by the run that chose Heap. Undoing that means
         * pointing the header back at the teardown that does not hand storage back, which is a
         * rewrite of a generated function rather than a field on an instruction. Until that is
         * worth doing, an environment keeps the answer it was given.
         */
        if(analysis.demoteOnly) {
            if(!slot.closureEnv && storage == StorageClass::Stack &&
               allocation.storage == StorageClass::Heap) {
                demoteToStack(analysis, allocation, slot);
            }

            continue;
        }

        /*
         * A closure environment is decided the same way as anything else, and released differently.
         *
         * The decision is the same because the question is: an environment is reachable from the
         * function value that owns it, so a closure that leaves this frame drags its captures along
         * and the containment closure in computeOutliving is what says so. A closure that is built,
         * called and dropped here does not, and there is nothing about being an environment that
         * makes the frame unable to hold it.
         *
         * What differs is who hands the storage back. Not this frame, whichever class it got: the
         * function value owns the environment, so freeing it at the end of this frame as well would
         * be a double free the moment the closure outlived one call. The closure's own derived
         * Reclaim does it, and it reads which class this was from the lambda's closure header -
         * which is why the decision is written back there rather than only into the IR.
         */
        if(slot.closureEnv) {
            allocation.storage = storage;
            allocation.releasedHere = false;

            /*
             * The heap answer, where it is the answer. The header is built holding the frame one,
             * so there is nothing to undo for an environment that stays here.
             *
             * Only on the run that rewrites, because this generates a function: the silent rounds
             * are an over-approximation being relaxed, and one of them deciding "heap" would leave
             * a release wrapper in the module that the settled answer does not want.
             */
            if(analysis.rewriting && storage == StorageClass::Heap && allocation.closure) {
                auto header = analysis.local[allocation.closure]->closureHeader;

                if(header) {
                    setClosureRelease(analysis.module, header,
                                      closureReleaseFor(analysis.module, slot.type, instruction.source));
                }
            }

            if(storage == StorageClass::Heap && analysis.module.program.allocateHeap) {
                analysis.local[analysis.module.program.allocateHeap]->used = true;
            }

            if(allocation.local < result.locals.size()) {
                result.locals[allocation.local].storage = storage;
            }

            /*
             * What is deliberately missing here is the write into the Local that the ordinary path
             * below makes.
             *
             * This pass runs once per fixpoint round, the first round reads the conservative answer
             * for every callee it has not summarized yet, and the slot the `@heap` override reads a
             * few lines up is the same one. Recording the decision there would make that first,
             * pessimistic round's answer the one every later round is forced back to - which is
             * exactly the round in which every closure looks like it escapes.
             */
            continue;
        }

        /*
         * Who releases it.
         *
         * Escaping and being handed over are not the same statement, and this is the line where the
         * difference is spent. Storage something else *owns* now is not released here: an array's
         * buffer is on the heap precisely because the array it belongs to left, and the array's own
         * `Drop` is what frees it, so releasing it here as well would free it twice. Storage that
         * escaped because a call this pass could not summarize may have kept a reference to it is
         * still this frame's, and the frame that stopped releasing it would leak it - which is what
         * `&counter` handed to a function value is, and what Analysis::transferred tells apart.
         *
         * A `@heap` binding is neither: it went to the heap because it was asked to, and it still
         * lives and dies in this frame.
         *
         * Storage whose class the program itself reads is a handover too, whatever the analysis
         * found: `storageFlag` exists so that another value's `Drop` can free this storage, and that
         * `Drop` is the one release it gets.
         *
         * A conditional move changes none of this, which is worth saying because it looks as though
         * it should. What a `->` argument hands over is what the slot *contained* - the storage
         * itself is this frame's on both paths, and stays this frame's - so a root the lattice
         * reached `Maybe` on is released here like any other. What the flag guards is the teardown;
         * the release runs either way. See analyze_drop.cpp's header.
         */
        allocation.releasedHere = !analysis.transferred[allocation.local] && !allocation.storageFlag &&
                                  !allocation.ownedElsewhere;
        allocation.storage = storage;

        /*
         * The flag the program reads at run time, where something asked for one - see
         * InstAlloc::storageFlag.
         *
         * "Is this the heap" and not the storage class itself, because that is the only thing anyone
         * asks: a `Reclaim` handed a run has four cases to tell apart and three of them do nothing,
         * for three reasons - the owner's own bytes, the frame returning, the region closing - that
         * are none of the value's business. Recording the whole class cost two bits of every
         * container's count word for a distinction nothing acts on, and taking the second bit back
         * doubled what a container can hold. See `HeapFlag` in native.cpp.
         */
        if(allocation.storageFlag && analysis.local[allocation.storageFlag]->kind == Value::ConstInt) {
            ((ConstInt*)analysis.local[allocation.storageFlag])->value = storage == StorageClass::Heap;
        }

        // Heap storage this frame owns has to be handed back at the end of the value's life, which
        // is a reason to drop a local whose type has no drop of its own.
        if(storage == StorageClass::Heap && allocation.releasedHere) {
            analysis.releasesStorage.set(allocation.local, true);
            analysis.tracked[allocation.local].droppable = true;
            if(allocation.local < result.locals.size()) result.locals[allocation.local].droppable = true;
        }

        if(storage == StorageClass::Heap && analysis.module.program.allocateHeap) {
            analysis.local[analysis.module.program.allocateHeap]->used = true;
        }

        // Only the storage class changes here, so the slot is written back with everything else it
        // held - including the two fields that are set by assignment rather than positionally.
        slot.storage = storage;
        analysis.function.locals.set(analysis.local, allocation.local, slot);

        if(allocation.local < result.locals.size()) result.locals[allocation.local].storage = storage;
    }
}
