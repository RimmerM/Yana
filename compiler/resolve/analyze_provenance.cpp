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
    return isMemoryType(analysis.global, type) || isPointer(analysis.global, type) ||
           isBorrow(analysis.global, type);
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

    for(Size i = 0; i < analysis.localCount; i++) {
        if(roots->locals[i]) joinProvenance(into, analysis.contents[i]);
    }

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
        for(Size i = 0; i < analysis.localCount; i++) {
            if(roots.locals[i]) joinProvenance(into, analysis.contents[i]);
        }

        if(roots.global || roots.unknown) into.unknown = true;
    } else if(refersToStorage(analysis, type)) {
        joinProvenance(into, provenanceOf(analysis, value));
    }
}

// The summary of a called function, or nothing when the callee is not one this pass can see.
FunctionSummary* summaryOf(Analysis& analysis, ModulePtr<Function> callee) {
    if(!callee) return nullptr;

    auto& summary = analysis.local[callee]->summary;
    return summary.ready && !summary.opaque ? &summary : nullptr;
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
        U16 index = 0;
        for(auto arg: args.contents(analysis.local)) {
            if(summary->declaredRoots & (U64(1) << min(U16(63), index))) {
                joinProvenance(into, provenanceOf(analysis, arg));
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

    U16 index = 0;
    for(auto arg: call.args.contents(analysis.local)) {
        if(signature->returnRoots & (U64(1) << min(U16(63), index))) {
            joinProvenance(into, provenanceOf(analysis, arg));
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
             * A borrow-like result also *contains* whatever the callee said it is rooted in.
             *
             * The slot is where the result landed, which is what the line above says and is the
             * whole answer for an owned aggregate: a returned record is a handover, and it refers to
             * nothing but the new storage it now occupies. A slice is not a handover - it is a
             * reference of memory type (isBorrowLike) - so the callee's declared root group has to
             * cross the call, and the slot having a backing local is exactly what stops the Call
             * case below from being reached to do it.
             */
            if(instruction.kind == Value::Call && isBorrowLike(analysis.module, instruction.type)) {
                auto& call = (InstCall&)instruction;
                callResultProvenance(analysis, call.callee, call.args, instruction.type, *part);
                changed = joinProvenance(analysis.contents[backing], *part) || changed;
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
                    // A cast of a pointer is the same address written differently, and asInt/asPtr
                    // are casts. Losing the root here is exactly how an escape would go unnoticed.
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

            for(Size l = 0; l < analysis.localCount; l++) {
                if(roots->locals[l]) changed = joinProvenance(analysis.contents[l], stored) || changed;
            }
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

    // Bounded rather than unbounded: each round can only add, and the lattice is finite, so this
    // settles - the bound is a guard against a rule added later that is not monotone, not a
    // shortcut. Loops need one round per level of the value graph they close over.
    for(Size round = 0; round < analysis.instructionCount + 2; round++) {
        if(!flowRound(analysis)) break;
    }
}
