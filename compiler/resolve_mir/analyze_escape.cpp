#include "analyze_internal.h"

namespace mir {

/*
 * Where every object goes - Analysis-IR.md §6.6 and §10.4, Implementation-Regions.md §4.
 *
 * One file for the whole of placement, and it is deliberately two things with a seam between them:
 * the **findings** (`Placement`), which several unrelated parts of the compiler establish, and the
 * **ranking** (`selectStorage`), which is one function nobody else duplicates. That is decision D6's
 * whole content. The shipping stage's equivalent grew the other way round - `selectStorage` there
 * reads six fields off an instruction and writes three of them back, and adding the region rung
 * would mean finding every one of those - so this stage states the split before there is a second
 * rung to get wrong.
 *
 * ## What establishes a finding, and what does not
 *
 * `Placement::demanded` is `@heap` on a binding, read by `readBindingAttributes` and carried here on
 * the `Local`. It is one-directional and always safe: it moves an object off the frame, never onto
 * it.
 *
 * `Placement::outlivesFrame` is the escape question, and the honest statement about it in *this*
 * representation is worth writing down, because the shipping stage's 800-line dataflow does not
 * translate: **in an ownership MIR nothing escapes by being a value.** A returned record is copied
 * into storage the caller passed in; an argument at a `->` position is moved into the callee's own
 * slot; a write into a global fills the global's storage. In every one of those the object travels
 * and its *storage* stays behind, which is the case the shipping stage spends `transferredProvenance`
 * separating out of a representation that cannot say it.
 *
 * What is left is storage reached through a **reference**, and every reference here is a loan. So the
 * escape seeds are exactly the loan checker's conformance failures - a loan in a returned value
 * (`checkReturnConformance`), a loan written into a place that outlives the frame
 * (`checkHandoverConformance`), a loan carried by a value at an owned position
 * (`checkCarriedOrigins`) - and those are computed, per position, by `analyze_loan.cpp` already. A
 * second dataflow here would be a second, weaker answer to a question that stage answers exactly.
 *
 * Which is why this pass does not have one. What it has is the seam: those three sites report today
 * *in terms of the placement they would need*, and the day the thing they refuse becomes buildable
 * they set this field instead of reporting. The one construct in the language that reaches them is a
 * closure whose environment outlives its frame, and what that is waiting for is not the rung - it is
 * a way for a `{code, env}` pair to find its environment's teardown at a drop site that cannot name
 * the environment's type. See `resolve_lambda.cpp`, where the refusals are, and
 * `doc/progress/2026-09-07-J24-heap-placement.md`.
 *
 * `Placement::regionWitness` is Implementation-Regions.md §2's ambient witness and is never set:
 * `GenContext` has no slot kind for a region, so there is nothing for a region-placed object to be
 * placed *in*. `verifyBody` refuses the rung for exactly that reason, and this pass cannot select it.
 *
 * ## Where the answer is spent, and why the drop walk never hears about it
 *
 * A placement is a fact about *storage*, and the analyses after this one are about *values*. So this
 * pass writes `Object::storage` and changes nothing else: the move paths, the initialization lattice
 * and the drop steps are identical at either rung, because what a value owes when it dies does not
 * depend on where it was sitting.
 *
 * The allocation and its release therefore live entirely below the MIR, which is the arrangement
 * `@box` already has (`resolve_ssa/lower/box.cpp` - "above this file the indirection does not
 * exist"). `Conversion::buildSlots` allocates a heap slot where it would have made a frame one, and
 * `Conversion::releaseSlots` hands it back at the function's exits. Those are the two ends of one
 * allocation per activation, which is what makes the pairing checkable rather than argued: an object
 * whose *value* left on some path still has its storage handed back on that path, and
 * `test/resolve/DropFlagHeap.yana` is that case measured against the allocator's bump pointer.
 *
 * The pass runs first among the analyses all the same, so that a reader looking for where placement
 * is decided finds it before anything that could be mistaken for deciding it.
 */

// The argument is in `place.h`, beside `Placement`.
StorageClass selectStorage(const Placement& asked) {
    /*
     * Cheapest first, which is the whole ranking.
     *
     * The frame unless something established otherwise; then the region where a witness names one -
     * this is Implementation-Regions.md §4's rung, and it is the one line that part adds here; then
     * the heap, which is what is left when the storage has to outlive everything cheaper.
     *
     * `demanded` is read first rather than folded into the rungs because it is not a finding about
     * the program - it is the author overruling one, and it may only overrule downward.
     */
    if(asked.demanded) return StorageClass::Heap;
    if(!asked.outlivesFrame) return StorageClass::Frame;
    if(asked.regionWitness) return StorageClass::Region;

    return StorageClass::Heap;
}

/*
 * Every object of one body, placed.
 *
 * Over the **locals** and joined onto the lineage, which is `Object`'s reason to exist: `let @heap a
 * = f(); let b = a` is one object with two names, and asking each name separately is how one
 * allocation comes to have two answers. `objectRoot` is the join.
 */
void Analysis::selectPlacements() {
    auto count = body.objects.size();
    if(!count) return;

    SmallArray<Placement, 16> asked;
    for(U32 i = 0; i < count; i++) asked.push(Placement());

    for(auto localPointer: body.locals.contents(base)) {
        auto local = base[localPointer];
        if(!local->demandsHeap) continue;

        auto root = body.objectRoot(base, local->object);

        // A binding whose object never came to exist - `readBindingAttributes` reports that at the
        // attribute, so there is nothing to say here and nothing to place.
        if(!root.valid()) continue;

        asked[root.index].demanded = true;
    }

    for(U32 i = 0; i < count; i++) {
        auto object = body.object(base, ObjectId(i));

        // The root's findings, for every member of the lineage: the answer is the object's and a
        // member is a name for it. Writing it into all of them keeps `storageOf` a lookup rather
        // than a walk for callers that already hold a non-root id.
        auto root = body.objectRoot(base, ObjectId(i));
        object->storage = selectStorage(asked[root.index]);

        // What tells the *MIR* reachability walk that the allocator is needed. `Program::
        // usesHeapPlacement` and not `usesBoxes`, which J51 split them for: a boxed field is
        // invisible to every later walk, while this is an `alloc` in a storage class that
        // `resolve_ssa/opt/reach.cpp` reads off the optimized SSA one at a time.
        if(object->storage != StorageClass::Frame) program.usesHeapPlacement = true;
    }
}

} // namespace mir
