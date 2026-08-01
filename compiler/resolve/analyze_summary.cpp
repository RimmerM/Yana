#include "analyze_pass.h"

/*
 * The summary: what a caller may know without looking at this body.
 *
 * Derived from the flow facts the passes before it computed rather than from a walk of its own,
 * which is the point of having built them: "is this argument retained" is "did anything derived from
 * its slot escape", and "where is the result rooted" is the provenance of what every `ret` handed
 * back.
 */

// Rebuilds the summary from the current round's facts, reporting whether anything moved. The
// fixpoint in analyze.cpp runs until every function in the program answers no.
bool deriveSummary(Analysis& analysis) {
    auto& function = analysis.function;
    auto& summary = function.summary;
    auto changed = !summary.ready;

    // Sized once and then updated in place: the fixpoint visits a function many times and the
    // module arena never gives anything back, so pushing per round would be a leak per round.
    while(summary.args.size() < function.args.size()) {
        summary.args.push(analysis.module.arena, ArgSummary());
    }

    U16 index = 0;
    U64 declared = 0;

    for(auto argPointer: function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];
        // The slot this parameter's storage is named by, or none for a scalar passed in a
        // register - which has no storage in this frame for anything to be rooted in.
        auto slot = backingLocal(analysis, (ModulePtr<Value>)argPointer);

        ArgSummary updated;
        updated.returnRoot = arg->returnRoot;

        if(slot != maxLimit<U32>) {
            updated.requirements = analysis.demand[slot];
            updated.retained = analysis.escaped[slot];
        }

        // A `&` parameter is a declaration that the caller's storage must be writable, whatever
        // this body turns out to do with it. The signature is the contract, not the body.
        if(arg->isMutableBorrow()) updated.requirements.mutation = MutationDemand::Writable;

        if(arg->returnRoot) declared |= rootBit(index);

        if(!(summary.args.get(analysis.local, index) == updated)) {
            summary.args.set(analysis.local, index, updated);
            changed = true;
        }

        index++;
    }

    // What every return path handed back, unioned. Provenance composition through a call already
    // happened when the call's own result got its provenance, so a function returning another
    // selector's result arrives here with that callee's roots already mapped through the operands.
    ScratchProvenance returned(analysis);
    ScratchProvenance leaving(analysis);
    auto returnsValue = false;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        if(instruction.kind != Value::Ret) continue;

        auto value = ((InstRet&)instruction).value;
        if(!value) continue;

        returnsValue = true;
        transferredProvenance(analysis, value, *leaving);
        joinProvenance(*returned, *leaving);
    }

    U64 actual = 0;
    auto invalid = returned->global || returned->unknown;

    for(Size l = 0; l < analysis.localCount; l++) {
        if(!returned->locals[l]) continue;

        auto slot = analysis.function.localAt(analysis.local, U32(l));
        auto arg = slot.value && analysis.local[slot.value]->kind == Value::Arg
            ? (Arg*)analysis.local[slot.value] : nullptr;

        // A borrow rooted in a sunk parameter is as invalid as one rooted in a local: the callee
        // owns what it was given, so there is no caller-side root left to keep it alive.
        if(arg && arg->convention != ast::BindType::Sink) actual |= rootBit(arg->index);
        else invalid = true;
    }

    auto bound = StorageBound::Frame;
    if(invalid) bound = StorageBound::Escapes;
    else if(actual) bound = StorageBound::Arguments;

    /*
     * A slice counts, and so does a record holding one - see containsBorrowLike.
     *
     * A slice is what a borrow of a container *is*, so a function handing one back is held to the
     * same return-root contract as one handing back a `&T`. And storing it in a record does not
     * launder it: `-> Cursor` where `Cursor` holds a `&[Int]` gives the caller a reference to
     * something, and which argument that something came from is exactly what a signature is for.
     */
    auto borrowed = containsBorrowLike(analysis.module, function.returnType);

    // Exclusivity is still a property of the borrow *type*, and a slice has none: §4.1 makes a
    // mutable slice a `&` binding of one rather than a second type, so there is nothing here to read.
    auto mutableResult = isBorrow(analysis.global, function.returnType) &&
        ((BorrowType*)analysis.global[function.returnType])->mut;

    // Everything reaching here is about the *result*, so a function that returns nothing keeps the
    // frame-bounded answer rather than inheriting a root from a path that returned no value.
    if(!returnsValue) {
        actual = 0;
        invalid = false;
        bound = StorageBound::Frame;
    }

    if(summary.declaredRoots != declared || summary.actualRoots != actual ||
       summary.invalidRoot != invalid || summary.resultBound != bound ||
       summary.returnsBorrow != borrowed || summary.mutableResult != mutableResult) {
        summary.declaredRoots = declared;
        summary.actualRoots = actual;
        summary.invalidRoot = invalid;
        summary.resultBound = bound;
        summary.returnsBorrow = borrowed;
        summary.mutableResult = mutableResult;
        changed = true;
    }

    summary.ready = true;
    return changed;
}
