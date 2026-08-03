#include "analyze_pass.h"

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

    for(Size l = 0; l < analysis.localCount; l++) {
        if(!roots.locals[l]) continue;

        if(analysis.escaped.add(l)) {
            analysis.outlives.set(l, true);
            changed = true;
        }

        // One root can be both, and being owned elsewhere is the stronger statement: a value handed
        // over is handed over however many other references to it were kept.
        if(kind == Escape::Owned && analysis.transferred.add(l)) changed = true;
    }

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

                for(Size l = 0; l < analysis.localCount; l++) {
                    auto slot = analysis.function.localAt(analysis.local, U32(l));
                    if(slot.value && analysis.local[slot.value]->kind == Value::Arg) {
                        leaving->locals.set(l, false);
                    }
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
                    changed = markEscaped(analysis, *written, Escape::Owned) || changed;
                }

                break;
            }

            case Value::Call: {
                auto& call = (InstCall&)instruction;
                auto summary = summaryOf(analysis, call.callee);
                U16 index = 0;

                for(auto arg: call.args.contents(analysis.local)) {
                    auto retained = !summary || index >= summary->args.size() ||
                                    summary->args.get(analysis.local, index).retained;

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
                 * Same reasoning as GenCall, and for a stronger reason: there is no callee at all to
                 * have a summary, so everything handed over is assumed kept.
                 *
                 * Assumed *kept*, though, and not assumed given away - which is the whole of what
                 * argumentEscape decides, and the difference between a root this frame still has to
                 * release and one it must not. A function value's arguments are the sharpest case
                 * for it precisely because nothing here can prove anything about them.
                 */
                auto& call = (InstCallDyn&)instruction;

                /*
                 * Except a `yield`, where the language answers the question the type could not.
                 *
                 * A continuation parameter is declared with the default convention and no `return`
                 * marker - synthesized that way for the `yield` form, and rejected outright
                 * otherwise: "it is called, not stored, and its extent is the call". So the value
                 * handed over is a borrow bounded by this instruction, and what the continuation
                 * body does with it is bounded by the ordinary borrow check the way every other
                 * borrowed parameter's use is. Nothing here has to know which continuation it is.
                 *
                 * This is a fact about the *declaration* rather than about a body, which is what
                 * makes it statable without a summary - and why it does not generalize to a
                 * function value that merely happens to declare a borrow. There, retention is a
                 * body fact and a borrowed parameter can genuinely escape; see deriveSummary,
                 * where `retained` is exactly `escaped[slot]`.
                 *
                 * See InstCallDyn::handover for what assuming otherwise cost.
                 */
                if(call.handover) break;

                for(auto arg: call.args.contents(analysis.local)) {
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
                 * signature, which has no body - summaryOf answers null for it, and the walk falls
                 * back to assuming everything is kept, which is what this case did for both.
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

                for(auto arg: call.args.contents(analysis.local)) {
                    auto retained = !summary || index >= summary->args.size() ||
                                    summary->args.get(analysis.local, index).retained;

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

        for(Size m = 0; m < analysis.localCount; m++) {
            // A root contains itself - that is how a parameter's contents are rooted in the
            // parameter - and that says nothing about escaping.
            if(m == l) continue;

            // Owned, always: being reachable through a root that outlives the frame is being
            // part of what that root's teardown releases.
            if(analysis.contents[l].locals[m] && !(analysis.escaped[m] && analysis.transferred[m])) {
                analysis.escaped.set(m, true);
                analysis.outlives.set(m, true);
                analysis.transferred.set(m, true);
                changed = true;
            }
        }
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
