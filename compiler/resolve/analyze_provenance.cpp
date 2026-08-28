#include "analyze_pass.h"

/*
 * Provenance and containment.
 *
 * Two facts computed together because the second needs the first: a value's provenance is the set of
 * roots it may refer to, and a root's contents are the provenance of everything written into it.
 * Everything the two interprocedural passes after this one state is stated over them - what has to
 * outlive the frame (analyze_escape.cpp) and what a caller may believe (analyze_summary.cpp).
 *
 * Both are "may" analyses climbing from empty, so a round that has not seen a callee's summary yet
 * is optimistic rather than wrong - flowRound only ever adds.
 */

bool joinProvenance(Provenance& target, const Provenance& source) {
    auto changed = target.locals.unionWith(source.locals);

    if((source.args & ~target.args) != 0) { target.args |= source.args; changed = true; }
    if(source.global && !target.global) { target.global = true; changed = true; }
    if(source.unknown && !target.unknown) { target.unknown = true; changed = true; }
    return changed;
}

Provenance& provenanceOf(Analysis& analysis, ModulePtr<Value> value) {
    static Provenance none;
    if(!value) return none;

    auto id = analysis.local[value]->id;
    return id < analysis.values.size() ? analysis.values[id] : none;
}

// Whether a value is the kind of thing that can refer to storage at all. A scalar computed into a
// register refers to nothing, and saying so keeps arithmetic out of the fixpoint entirely.
bool refersToStorage(Analysis& analysis, TypePtr type) {
    if(isMemoryType(analysis.global, type) || isPointer(analysis.global, type) ||
       isBorrow(analysis.global, type)) {
        return true;
    }

    /*
     * A *register* type that still holds a reference, which is a shape the three tests above cannot
     * see: `Maybe(&T)` is one word once Repr folds `Nothing` into the null address, so it is direct,
     * it is not itself a borrow, and it names the callee's argument storage exactly as a bare `&T`
     * would.
     *
     * Missing it is a use-after-free rather than a missed optimization. `find(m, key) -> Maybe(&v)`
     * declares `return self`, and with no provenance to carry the drop of `m` was placed at the
     * call - so the borrow the caller then read through named storage that had already been handed
     * back. What fixed that case was `analyze_effects.cpp`'s walk out of the local a call wrote its
     * result into; this is the same fact stated where provenance reads it, so that escape analysis
     * cannot place on a frame storage something reaches through one of these either.
     *
     * Asked last because it walks the type: everything above it is one comparison, and a memory type
     * has already answered yes without needing the walk.
     */
    return containsBorrowLike(analysis.module, type);
}

// The roots a place names. A projection stays inside the storage its root names, so the path is
// not walked - which is the same reason liveness is tracked per local.
void placeProvenance(Analysis& analysis, const Place& place, Provenance& into) {
    into.reset(analysis.localCount);

    switch(place.root) {
        case PlaceRoot::Local:
            if(place.local < analysis.localCount) into.locals.set(place.local, true);
            else into.unknown = true;
            break;

        case PlaceRoot::Global:
            into.global = true;
            break;

        case PlaceRoot::Pointer:
        case PlaceRoot::Borrow:
            // The place is the memory the pointer names, so its roots are the pointer's own. A
            // borrow answers the same way: how much was *proved* about the address is what separates
            // the two roots, and provenance is not one of the things it separates.
            joinProvenance(into, provenanceOf(analysis, place.pointer));
            break;
    }
}

// What reading out of a place produces: everything anything ever wrote into the roots it names.
void contentsOfPlace(Analysis& analysis, const Place& place, Provenance& into) {
    ScratchProvenance roots(analysis);
    placeProvenance(analysis, place, *roots);

    into.reset(analysis.localCount);

    // Over the roots the place actually named rather than over every local the frame has - see
    // IndexSet::forEach. A place names one root in nearly every case and this runs per instruction
    // per round, so the difference is the frame's width times the fixpoint.
    roots->locals.forEach([&](Size i) { joinProvenance(into, analysis.contents[i]); });

    if(roots->global || roots->unknown) into.unknown = true;
}

// What a value contributes when it is written somewhere. An aggregate is copied byte for byte, so
// what lands in the destination is what the source contained rather than the source itself.
void transferredProvenance(Analysis& analysis, ModulePtr<Value> value, Provenance& into) {
    into.reset(analysis.localCount);
    if(!value) return;

    auto type = analysis.local[value]->type;

    if(isMemoryType(analysis.global, type)) {
        auto& roots = provenanceOf(analysis, value);
        roots.locals.forEach([&](Size i) { joinProvenance(into, analysis.contents[i]); });

        if(roots.global || roots.unknown) into.unknown = true;
    } else if(refersToStorage(analysis, type)) {
        joinProvenance(into, provenanceOf(analysis, value));
    }
}

/*
 * The roots a *result* has, with every frame slot that is only holding a view replaced by what that
 * slot refers to.
 *
 * The fixpoint composes one step per call, and this closes the chain. `slice(elements(xs), a, b)`
 * is the shape: `elements` hands back a descriptor rooted in `xs`, the descriptor lands in a slot of
 * this frame, and `slice` borrows that slot and hands back a window rooted in the same array. What
 * arrives at the return is therefore a root that is a **local**, and both passes that ask about a
 * result read a local root as "this frame's storage" - so a window reaching its root through two
 * calls was refused by checkReturnRoots and its descriptor was placed on the heap by
 * computeOutliving, while either call alone was fine. Every view-returning function in the library
 * has that shape.
 *
 * **Through `contents` rather than through `Local::viewOf`.** A descriptor built in this frame out
 * of a local array carries the `viewOf` link, and `viewedRoot` is still the first step here for that
 * case. One a *call* filled carries nothing, because what says where it points is the callee's
 * declared group - which is exactly what `callResultProvenance` has already written into the slot's
 * contents. Reading the contents is also the direction Analysis-Borrows.md §6.6 states: the
 * correctness uses of `viewOf` are what a typed loan slot is meant to retire, so a second one added
 * here would be a step away from it.
 *
 * **The guard is that the slot holds a view and owns nothing**, which is checkReturnRoots'
 * `ownsNothing` asked of a slot instead of a result and for the same reason. A slot that owns its
 * contents is where a root legitimately is - the storage goes with the result - and following one
 * would make every result built out of an owned local look rooted in whatever filled that local. A
 * view slot that nothing known was written into stays its own root rather than resolving to none at
 * all: an empty answer would be silence where a local root gives a diagnostic.
 */
void resolveViewRoots(Analysis& analysis, const Provenance& from, Provenance& into) {
    into.reset(analysis.localCount);
    into.args = from.args;
    into.global = from.global;
    into.unknown = from.unknown;

    // Deduplicated on the way in, which is what bounds the walk: a slot resolved once is never
    // queued again, so the length of this is the frame's width however the views are chained.
    SmallArray<U32, 8> pending;
    auto reach = [&](Size l) {
        if(l >= analysis.localCount) { into.unknown = true; return; }

        for(auto seen: pending) { if(seen == U32(l)) return; }
        pending.push(U32(l));
    };

    from.locals.forEach([&](Size l) { reach(l); });

    for(Size i = 0; i < pending.size(); i++) {
        auto local = pending[i];
        auto slot = analysis.function.localAt(analysis.local, local);

        /*
         * A slot a *call* filled, which is the whole of what this resolves and is deliberately not
         * "any slot holding a reference".
         *
         * Two other shapes hold one and are not steps along a chain. A closure environment holds a
         * by-reference capture, so it contains a borrow and owns nothing - and it is nevertheless
         * real storage that the function value returned beside it points *at*, rather than two words
         * copied out at the return. And a descriptor `convertSlice` built out of a local array is
         * the slot `checkEscapingViews` names in its diagnostic, reached through `Local::viewOf` by
         * the consumers rather than here. Both are defined by something other than a call, so
         * requiring one leaves each of them exactly where it was.
         */
        auto filled = slot.value ? analysis.local[slot.value] : nullptr;
        auto fromCall = filled && (filled->kind == Value::Call || filled->kind == Value::GenCall ||
                                   filled->kind == Value::CallDyn);

        auto view = fromCall && slot.type && containsBorrowLike(analysis.module, slot.type) &&
                    !needsTeardown(analysis.module, slot.type);

        if(!view) {
            into.locals.set(local, true);
            continue;
        }

        auto& written = analysis.contents[local];
        if(!written.args && !written.global && !written.unknown && !written.locals.popCount()) {
            into.locals.set(local, true);
            continue;
        }

        into.args |= written.args;
        into.global = into.global || written.global;
        into.unknown = into.unknown || written.unknown;

        written.locals.forEach([&](Size l) { reach(l); });
    }
}

// The summary of a called function, or nothing when the callee is not one this pass can see.
FunctionSummary* summaryOf(Analysis& analysis, ModulePtr<Function> callee) {
    if(!callee) return nullptr;

    auto& summary = analysis.local[callee]->summary;
    return summary.ready && !summary.opaque ? &summary : nullptr;
}

/*
 * The loan group a result is in, or `kNoLoan` where reading it would not narrow anything.
 *
 * Analysis-Borrows.md §4.2: a signature's groups exist to say that two results carry loans of
 * *different* parameters, so a result in one group is related to that group's members and to
 * nothing else. This is the query that spends them, and the only one below resolve that does - a
 * group has no runtime meaning (§8.2), so nothing after this reads it.
 *
 * Only a result that *is* a reference answers. A result that merely contains one - `Maybe('v)`, a
 * record of two - is field-insensitive here exactly as provenance is everywhere else, and the
 * honest answer for it is the one that was always given: every group at once. That keeps §4.2's
 * `both` correct rather than precise, and precision for it needs the same slot map §6.6 wants.
 *
 * `kNoLoan` therefore means "join every group", not "join none". A signature with one group - which
 * is every signature in `lib/`, per §4.8 - reaches the same set either way, which is what makes
 * this a narrowing of the new case and not a change to any existing one.
 */
static LoanGroup resultLoanGroup(Analysis& analysis, TypePtr result) {
    if(!result) return kNoLoan;

    auto type = analysis.global[result];
    return type->kind == Type::Borrow ? ((BorrowType*)type)->loan : kNoLoan;
}

// Whether one argument's loan is part of what a result in `group` may refer to. An argument in no
// group cannot root anything; past that, `kNoLoan` on the result is the "cannot tell" above.
static bool rootsResult(LoanGroup arg, LoanGroup group) {
    if(arg == kNoLoan) return false;
    return group == kNoLoan || arg == group;
}

// What a call's result may refer to, composed from the callee's declared return-root group. A
// borrow coming out of a call is related to every member of that group at once, which is
// Design.md's deliberate conservatism: the callee may have returned any of them.
static void callResultProvenance(Analysis& analysis, ModulePtr<Function> callee,
                                 ModuleList<ModulePtr<Value>, false>& args, TypePtr type,
                                 Provenance& into) {
    into.reset(analysis.localCount);
    if(!refersToStorage(analysis, type)) return;

    auto summary = summaryOf(analysis, callee);
    if(!summary) {
        into.unknown = true;
        return;
    }

    if(summary->resultBound == StorageBound::Arguments) {
        auto declared = analysis.local[callee];
        auto group = resultLoanGroup(analysis, type);
        U16 index = 0;

        for(auto arg: args.contents(analysis.local)) {
            if(summary->declaredRoots & (U64(1) << min(U16(63), index))) {
                auto parameter = index < declared->args.size()
                    ? analysis.local[declared->args.get(analysis.local, index)]->loan : kNoLoan;

                if(rootsResult(parameter, group)) joinProvenance(into, provenanceOf(analysis, arg));
            }

            index++;
        }
    } else if(summary->resultBound != StorageBound::Frame) {
        into.unknown = true;
    }
}

/*
 * The same, for a call through a function value.
 *
 * The signature is what a caller reaching a function this way has, and FunArg carries the `return`
 * marker precisely so that it is enough: the group is declared on the *type*, so a borrow coming out
 * of the call is related to the arguments in that group exactly as a direct call's result is related
 * to its callee's. Falling back to `unknown` here would be reading the contract and then ignoring it.
 *
 * Null signature - a teardown the compiler calls through a descriptor - has no contract to read, and
 * a result that refers to storage is then storage this analysis cannot name.
 */
static void dynamicResultProvenance(Analysis& analysis, InstCallDyn& call, Provenance& into) {
    into.reset(analysis.localCount);
    if(!refersToStorage(analysis, call.type)) return;

    auto signature = call.signature && analysis.global[call.signature]->kind == Type::Fun
        ? (FunType*)analysis.global[call.signature] : nullptr;

    if(!signature || !signature->returnRoots) {
        into.unknown = true;
        return;
    }

    auto group = resultLoanGroup(analysis, call.type);
    U16 index = 0;

    for(auto arg: call.args.contents(analysis.local)) {
        if(signature->returnRoots & (U64(1) << min(U16(63), index))) {
            if(rootsResult(signature->args.get(analysis.global, index).loan, group)) {
                joinProvenance(into, provenanceOf(analysis, arg));
            }
        }

        index++;
    }
}

// One round of the value fixpoint. Returns whether anything was added.
static bool flowRound(Analysis& analysis) {
    auto changed = false;
    auto local = analysis.local;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto pointer = analysis.order[i];
        auto& instruction = *local[pointer];
        auto id = instruction.id;
        if(id >= analysis.values.size()) continue;

        ScratchProvenance produced(analysis);

        // What each case below composes its answer out of, borrowed once for the whole switch
        // rather than per case - nothing here needs two of them at a time.
        ScratchProvenance part(analysis);

        // A value that owns a slot refers to that slot and to nothing else, whatever produced it -
        // an allocation, a copy, a call whose aggregate result landed in one.
        auto backing = backingLocal(analysis, (ModulePtr<Value>)pointer);
        if(backing != maxLimit<U32>) {
            produced->locals.set(backing, true);

            /*
             * A result that *contains* a reference also contains whatever the callee said it is
             * rooted in.
             *
             * The slot is where the result landed, which is what the line above says and is the
             * whole answer for an aggregate holding nothing but owned members: a returned record is
             * a handover, and it refers to nothing but the new storage it now occupies.
             *
             * It is not the whole answer for one holding a reference, and testing `isBorrowLike`
             * here made that the same case. A slice passed the test - it is a reference of memory
             * type - and a record with a slice or a `&T` in it did not, so the callee's declared
             * root group stopped crossing the call at the first aggregate that wrapped it. Two
             * levels was enough to lose it: `fn d1(n: 'Int) -> L1 = L1 {r: n}` is an Aggregate and
             * is caught by the component writes below, but `fn d2(n: 'Int) -> L2 = L2 {a: d1(n)}`
             * reads `d1`'s result through this path, and with no roots joined here `deriveSummary`
             * saw a result rooted in nothing and let the signature hand back a reference it never
             * declared. Analysis-Borrows.md §2.4's "provenance can be lost by repackaging a
             * reference into a generic aggregate", exactly.
             *
             * `containsBorrowLike` rather than `isBorrowLike`, so the two are one question: it tests
             * `isBorrowLike` first, so a slice still answers yes for the reason it always did.
             */
            if(containsBorrowLike(analysis.module, instruction.type)) {
                if(instruction.kind == Value::Call) {
                    auto& call = (InstCall&)instruction;
                    callResultProvenance(analysis, call.callee, call.args, instruction.type, *part);
                    changed = joinProvenance(analysis.contents[backing], *part) || changed;
                } else if(instruction.kind == Value::GenCall) {
                    /*
                     * No summary to read, so the conservative answer the GenCall case below already
                     * gives: anything this call was handed. Written here as well because a result of
                     * memory type never reaches that case - the backing slot is what diverts it.
                     */
                    part->reset(analysis.localCount);

                    for(auto arg: ((InstGenCall&)instruction).args.contents(local)) {
                        joinProvenance(*part, provenanceOf(analysis, arg));
                    }

                    changed = joinProvenance(analysis.contents[backing], *part) || changed;
                }
            }
        } else {
            switch(instruction.kind) {
                case Value::LoadPlace: {
                    auto& read = (InstLoadPlace&)instruction;

                    // An aggregate is addressed rather than loaded, so the value *is* the place.
                    // A scalar or a pointer read out of storage is whatever was written there.
                    if(isMemoryType(analysis.global, instruction.type)) {
                        placeProvenance(analysis, read.place, *part);
                        joinProvenance(*produced, *part);
                    } else if(refersToStorage(analysis, instruction.type)) {
                        contentsOfPlace(analysis, read.place, *part);
                        joinProvenance(*produced, *part);
                    }

                    break;
                }

                case Value::Borrow:
                    placeProvenance(analysis, ((InstBorrow&)instruction).place, *part);
                    joinProvenance(*produced, *part);
                    break;

                case Value::Address:
                    placeProvenance(analysis, ((InstAddress&)instruction).place, *part);
                    joinProvenance(*produced, *part);
                    break;

                case Value::Move:
                    placeProvenance(analysis, ((InstMove&)instruction).place, *part);
                    joinProvenance(*produced, *part);
                    break;

                // What came out of the place may refer to whatever the place did. Only reached for a
                // scalar: an aggregate result has a slot of its own, which backingLocal answers with
                // before this switch runs.
                case Value::Exchange:
                    placeProvenance(analysis, ((InstExchange&)instruction).place, *part);
                    joinProvenance(*produced, *part);
                    break;

                case Value::Cast:
                case Value::Bitcast:
                    // A cast of a pointer is the same address written differently, and reading one
                    // as an integer or back is a `Bitcast`. Losing the root here is exactly how an
                    // escape would go unnoticed.
                    joinProvenance(*produced, provenanceOf(analysis, ((InstUnary&)instruction).from));
                    break;

                case Value::Add:
                case Value::Sub:
                    // Pointer arithmetic stays inside whatever the pointer named.
                    if(refersToStorage(analysis, instruction.type)) {
                        joinProvenance(*produced, provenanceOf(analysis, ((InstBinary&)instruction).lhs));
                        joinProvenance(*produced, provenanceOf(analysis, ((InstBinary&)instruction).rhs));
                    }

                    break;

                /*
                 * Either arm, which is the same join a phi of the two would make - and it has to be
                 * here for the same reason: `let p = if c then &a else &b` is a diamond until
                 * `convertSelects` collapses it, and a body reaching this analysis can already hold
                 * the collapsed form. `settle` runs the whole optimizer round on a callee before the
                 * inliner judges a site against it, and the inliner is what runs `reselectStorage`.
                 *
                 * The condition contributes nothing: it decides which reference this is, not what
                 * either of them refers to.
                 */
                case Value::Select: {
                    auto& select = (InstSelect&)instruction;
                    joinProvenance(*produced, provenanceOf(analysis, select.whenTrue));
                    joinProvenance(*produced, provenanceOf(analysis, select.whenFalse));
                    break;
                }

                case Value::Call: {
                    auto& call = (InstCall&)instruction;
                    callResultProvenance(analysis, call.callee, call.args, instruction.type, *part);
                    joinProvenance(*produced, *part);
                    break;
                }

                case Value::CallDyn:
                    /*
                     * There is no callee to have a summary, by construction: which function this
                     * reaches is what a function value decides at run time. What there is instead is
                     * the signature the call was written through, and its declared `return` group is
                     * the one thing a caller in this position may believe - see
                     * dynamicResultProvenance. Where it declares nothing, the result refers to
                     * storage this analysis cannot name, which is the answer Design-Memory §13 gives.
                     */
                    dynamicResultProvenance(analysis, (InstCallDyn&)instruction, *part);
                    joinProvenance(*produced, *part);
                    break;

                case Value::GenCall:
                    /*
                     * No summary to read - the instance is decided per specialization - so the
                     * conservative reading of what a reference result may point at is: anything
                     * this call was handed.
                     *
                     * Deliberately without `unknown`, which would be the safer answer anywhere
                     * else. An InstGenCall never survives specialization, and every specialization
                     * is checked in full with the real instructions in place; so what this decides
                     * is only how much a *generic* body is allowed to say about itself, and the
                     * soundness of any concrete program is settled elsewhere. Saying `unknown` here
                     * would instead make every generic accessor unable to declare its own roots.
                     */
                    if(refersToStorage(analysis, instruction.type)) {
                        for(auto arg: ((InstGenCall&)instruction).args.contents(local)) {
                            joinProvenance(*produced, provenanceOf(analysis, arg));
                        }
                    }

                    break;

                case Value::Native:
                    // copyMemory and setMemory produce nothing, and a syscall produces an integer.
                    // Neither hands back an address this analysis could have named.
                    if(refersToStorage(analysis, instruction.type)) produced->unknown = true;
                    break;

                case Value::Phi:
                    for(auto input: ((InstPhi&)instruction).inputs.contents(local)) {
                        joinProvenance(*produced, provenanceOf(analysis, input.value));
                    }

                    break;

                default:
                    break;
            }
        }

        changed = joinProvenance(analysis.values[id], *produced) || changed;

        // Writing into a place makes what was written reachable through that place's root.
        auto storeInto = [&](const Place& place, const Provenance& stored) {
            ScratchProvenance roots(analysis);
            placeProvenance(analysis, place, *roots);

            roots->locals.forEach([&](Size l) {
                changed = joinProvenance(analysis.contents[l], stored) || changed;
            });
        };

        /*
         * A stored reference makes its owner refer to the reference's own slot as well.
         *
         * transferredProvenance answers what a value *contributes*, and for an aggregate that is
         * what it contained rather than the slot it sat in - which is right, because writing a
         * record copies it. A slice is a borrow whose representation is a record (isBorrowLike), and
         * the descriptor's slot is what carries `Local::viewOf`, so without this edge
         * `Cursor {items: xs}` reaches the run and never the descriptor and checkEscapingViews has
         * nothing to see leaving.
         *
         * Only here, and not in transferredProvenance itself. A slice *returned* hands over a copy
         * of two words, so what outlives is what those words point at and not the slot they were in
         * - adding the slot there makes every subslice look rooted in its own frame.
         */
        auto storedReference = [&](ModulePtr<Value> value, Provenance& target) {
            if(!value || !isBorrowLike(analysis.module, analysis.local[value]->type)) return;
            joinProvenance(target, provenanceOf(analysis, value));
        };

        if(instruction.kind == Value::Init || instruction.kind == Value::Assign) {
            auto& write = (InstInit&)instruction;
            transferredProvenance(analysis, write.value, *part);
            storedReference(write.value, *part);
            storeInto(write.place, *part);
        } else if(instruction.kind == Value::Aggregate) {
            /*
             * The same, per component - an aggregate is the writes it replaces and nothing else.
             *
             * Its own place is *not* stored into as a whole. Every component is a step off it and
             * `placeProvenance` reaches a root through any path, so writing the components says what
             * writing the value said; writing the whole place as well would additionally claim the
             * aggregate refers to whatever any one component does, which is what the per-field
             * `Init`s never claimed.
             *
             * Leaving this case out was a wrong answer rather than a lost one: `Chunked` puts a
             * slice in a field, the descriptor's slot is what carries `Local::viewOf`, and without
             * the edge the escape analysis placed a container's storage in a frame it outlived.
             */
            auto& aggregate = (InstAggregate&)instruction;

            eachWrittenComponent(local, analysis.module.arena, aggregate,
                                 [&](Place place, ModulePtr<Value> value, Size) {
                ScratchProvenance stored(analysis);
                transferredProvenance(analysis, value, *stored);
                storedReference(value, *stored);
                storeInto(place, *stored);
            });
        } else if(instruction.kind == Value::Exchange) {
            auto& exchange = (InstExchange&)instruction;
            transferredProvenance(analysis, exchange.value, *part);
            storeInto(exchange.place, *part);
        } else if(instruction.kind == Value::Swap) {
            /*
             * Each place ends up holding what the other did. The sets are joined both ways rather
             * than crossed over, because this is a fixpoint over a join lattice and there is no
             * "used to hold" to take away - a place that ever held either is a place that may refer
             * to either, which is the answer escape analysis needs and the only one it can keep.
             */
            auto& swap = (InstSwap&)instruction;
            ScratchProvenance both(analysis);

            contentsOfPlace(analysis, swap.a, *both);
            contentsOfPlace(analysis, swap.b, *part);
            joinProvenance(*both, *part);

            storeInto(swap.a, *both);
            storeInto(swap.b, *both);
        }
    }

    return changed;
}

void computeProvenance(Analysis& analysis) {
    analysis.valueCount = analysis.function.valueCounter;

    analysis.values.reset(analysis.valueCount, analysis.localCount);
    analysis.contents.reset(analysis.localCount, analysis.localCount);

    /*
     * What is inside a parameter is rooted in that parameter.
     *
     * Nothing in this frame wrote it, so without this a pointer loaded out of an argument would
     * come from nowhere and an accessor could not say what its result was rooted in. "Reachable
     * through" is exactly the relation Design.md's rule is stated over - "every borrow escaping
     * through the result must be transitively rooted in a `return` parameter" - so a parameter's
     * contents starting as the parameter itself is that rule's base case.
     */
    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(!slot.value || analysis.local[slot.value]->kind != Value::Arg) continue;

        analysis.contents[l].locals.set(l, true);

        /*
         * And the parameter *value* refers to that slot.
         *
         * flowRound cannot say it: an Arg is not an instruction, so it is never visited and its slot
         * in `values` stays empty. That was invisible while every memory-typed result was an owned
         * one - a returned record is a handover, and a provenance of nothing is the right answer for
         * it. It stopped being invisible with Implementation-Containers.md §4's slice, where
         * `fn f(xs: [Int]) -> &[Int] = xs` hands back a *reference* of memory type: the value's
         * provenance was empty, so deriveSummary saw no roots and the return-root check had nothing
         * to object to.
         */
        auto id = analysis.local[slot.value]->id;
        if(id < analysis.values.size()) analysis.values[id].locals.set(l, true);
    }

    /*
     * And a parameter with no slot at all refers to *itself* - see Provenance::args.
     *
     * The loop above is over locals, so it reaches a parameter only where the frame gave it one:
     * `&x: T` names storage the caller owns and gets a slot holding that address, and an argument
     * of memory type gets one because that is where it was copied to. A parameter whose declared
     * type is itself a reference gets neither - `&Int` and `%U8` are direct types, so they arrive in
     * a register and `resolveArgs` makes no local - and the two of them are exactly the parameters
     * whose *value* is an address into the caller.
     *
     * So they had no provenance whatever, and a result derived from one was rooted in nothing. That
     * is not a missed optimization: `deriveSummary` reads "no roots" as "the result is bounded by
     * this frame", so the return-root check had nothing to object to and the signature was free to
     * hand back a reference it never declared. Both halves of it were reachable from source -
     * `Pair(&Int, &Int)` built out of two `&Int` parameters, and a `%U8` handed straight back.
     *
     * Rooted in the argument index rather than in a slot, because there is no slot to root it in.
     * Everything downstream composes the two the same way: `joinProvenance` carries both, and
     * `deriveSummary` turns each into the same bit of the same mask.
     */
    for(auto argPointer: analysis.function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];
        if(!isBorrow(analysis.global, arg->type) && !isPointer(analysis.global, arg->type)) continue;
        if(backingLocal(analysis, (ModulePtr<Value>)argPointer) != maxLimit<U32>) continue;

        if(arg->id < analysis.values.size()) analysis.values[arg->id].args |= rootBit(arg->index);
    }

    // Bounded rather than unbounded: each round can only add, and the lattice is finite, so this
    // settles - the bound is a guard against a rule added later that is not monotone, not a
    // shortcut. Loops need one round per level of the value graph they close over.
    for(Size round = 0; round < analysis.instructionCount + 2; round++) {
        if(!flowRound(analysis)) break;
    }
}
