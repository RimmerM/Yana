#include "opt_pass.h"

/*
 * Discharging ownership - Implementation-Simplification.md §14.
 *
 * `compiler/opt` runs over the resolve IR after every ownership question has been decided and
 * spent, and `opt.h` argues correctly that this is what makes rewriting it safe. What it costs is
 * that the optimizer must *preserve* the instructions those decisions became, and `clonableKind` is
 * that constraint written down: a body containing one is declined outright, so the shapes worth
 * inlining most - anything over a type with a teardown - were the shapes that could not be.
 *
 * Native escapes it because `resolve/lower.cpp` discharges ownership on the way to the machine.
 * This is the same expansion, one stage earlier, where both targets and the optimizer can see it.
 *
 * ## What is discharged, and what is not
 *
 * `Drop` is, into the calls it stands for. `Move` deliberately is **not**: it is the relocation
 * itself rather than a decision about one - `codegen/js` emits nothing for it but the name, and
 * reads its kind to alias where a load would deep-clone - so there is no lower form to turn it into.
 * It is admitted to `clonableKind` instead. `Swap` and `Exchange` are not discharged yet; they are
 * ordinary expansions with nothing in the way, and until they are done a body containing one is
 * declined exactly as it is today.
 *
 * `Init`, `Assign`, `Borrow` and `Alloc` are not ownership instructions by the time this runs and
 * were never on the list - see §7's "deliberately not discharged".
 *
 * ## The postcondition is per function, not per program
 *
 * A generic body's drop reaches its type's teardown through the caller's descriptor, which is a
 * `genSlot` - a lowering-level value with no representation in this IR - so an erased `Drop` cannot
 * be discharged at all. That bounds this pass; it does not block it. Every consumer of the
 * postcondition is per callee, and `opt_inline`'s `describe` refuses `callee->gen` outright, so an
 * erased body is never in a candidate. What holds afterwards is:
 *
 *     a non-generic body contains no `Drop`
 *
 * and that is the statement `clonableKind` needs.
 *
 * ## Why this runs inside the optimized build rather than in front of both
 *
 * `ResolveTester` compiles every fixture with a run expectation twice, optimized and unoptimized,
 * and compares the two answers with *each other* rather than with any file. Running the discharge
 * only on the optimized side makes that check assert this pass: one side reaches the teardown
 * through `InstDrop` and the backend's own lowering of it, the other through the calls written here,
 * and the two have to agree about how many times a value was released. Neither side of that
 * comparison is a golden file, so it cannot be regenerated into passing.
 */

namespace {

struct Discharge {
    OptContext& opt;

    // A managed target's collector owns reclamation, so the reclaim half and the storage release are
    // both nothing there - the same carve-out Design-Memory §4 states and `genDrop` applies. What is
    // left is the drop half, which is an effect at last use and is never elided on any target.
    bool collected;

    /*
     * One drop, as the calls it stands for.
     *
     * The order is `resolve/lower.cpp`'s and is the only one that works: whatever a teardown does,
     * it does while the storage is still there to do it in, so the release is last. And a reclaim
     * that names the same function as the drop is one traversal serving both halves
     * (Implementation-Containers.md §13) - running it twice would release every element twice, which
     * is invisible in the IR and shows up as a counter going negative.
     */
    bool dischargeDrop(Block& block, Size index, InstDrop& drop) {
        auto& module = *opt.module;
        auto& function = *opt.function;
        auto source = drop.source;
        auto unit = opt.program.scalar.unit;

        // A drop the pass should have elided rather than emitted. Left alone rather than asserted
        // against, since this stage is not the one that gets to decide the drop pass was wrong.
        if(drop.isEmpty()) return false;

        /*
         * A conditional teardown is the one shape with no discharged form yet.
         *
         * The flag stands for a block split and a test around the calls below, which is real CFG
         * work and is the thing drop flags need in general - see analyze.cpp's closing list. Nothing
         * emits one today (they are reported instead), so this is the guard that keeps it that way
         * rather than a path anything takes.
         */
        if(drop.flag != maxLimit<U32>) return false;

        auto type = placeType(module, function, drop.place);
        if(!type) return false;

        /*
         * The erased half, which has no expression here - see the header. A body that cannot see the
         * shape of what it is dropping reaches the teardown through its caller's descriptor, and the
         * descriptor is not a value of this IR.
         *
         * Asked of the *place's* type rather than of the function, because a generic body drops
         * concrete things too - a `Maybe(Int)` it built itself is dischargeable inside a body whose
         * `a` is not.
         */
        if(isGeneric(opt.global, type)) return false;

        InstList replacement;

        /*
         * The subject, read once and handed to each half.
         *
         * A teardown takes its subject by `->` (Implementation-Simplification.md §13), so what a
         * call site passes is the value - which natively is the address of a memory type's storage
         * and on a managed target is the host value. One load serves both halves because neither
         * ends the storage: the release below is what does, and it comes last.
         */
        ModulePtr<Value> subject = nullptr;
        auto subjectOf = [&]() {
            if(!subject) {
                auto loaded = createInst<InstLoadPlace>(module, function, block, source, 0, type,
                                                        drop.place);
                replacement.push(loaded);
                subject = (ModulePtr<Value>)(loaded - opt.local);
            }

            return subject;
        };

        auto step = [&](ModulePtr<Function> callee) {
            if(!callee) return;

            opt.local[callee]->used = true;

            auto call = createInst<InstCall>(module, function, block, source, 0, unit, callee);
            call->args.push(module.arena, subjectOf());
            replacement.push(call);
        };

        step(drop.drop);
        if(!collected && drop.reclaim != drop.drop) step(drop.reclaim);

        /*
         * Handing the allocation back, after both halves have finished reading it.
         *
         * The address rather than the value, and `freeHeap` is written over `%U8` - so the pointer
         * is reinterpreted rather than converted, both sides being one machine word with only what
         * the program says they mean differing. The same cast `closureReleaseFor` makes.
         */
        if(!collected && drop.releaseStorage && opt.program.freeHeap) {
            auto free = opt.local[opt.program.freeHeap];
            free->used = true;

            auto pointerType = resolvePointerType(module, type);
            auto address = createInst<InstAddress>(module, function, block, source, 0, pointerType,
                                                   drop.place);
            replacement.push(address);

            auto argument = (ModulePtr<Value>)(address - opt.local);
            auto expected = free->args.isEmpty() ? pointerType
                                                 : opt.local[free->args.get(opt.local, 0)]->type;

            if(!sameType(expected, pointerType)) {
                auto cast = createInst<InstUnary>(module, function, block, source, 0, expected,
                                                  Value::Cast, argument);
                replacement.push(cast);
                argument = (ModulePtr<Value>)(cast - opt.local);
            }

            auto call = createInst<InstCall>(module, function, block, source, 0, unit,
                                             opt.program.freeHeap);
            call->args.push(module.arena, argument);
            replacement.push(call);
        }

        // A drop whose every half was the collector's is nothing at all here, and removing it is the
        // whole of what it discharges to.
        insertInstructions(opt, block, index, replacement);
        eraseInstruction(opt, (ModulePtr<Inst>)(&drop - opt.local));

        opt.changed = true;
        return true;
    }

    void run(Function& function) {
        opt.function = &function;

        for(auto blockPointer: function.blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            /*
             * By index and downwards, because the list is rewritten under the walk: the replacement
             * goes in at `index` and the drop that was there is erased afterwards, so anything at or
             * before `index` has moved. Going backwards means the positions still to visit are the
             * ones nothing has touched.
             */
            for(Size i = block->instructions.size(); i-- > 0;) {
                auto pointer = block->instructions.get(opt.local, i);
                auto& instruction = *opt.local[pointer];

                if(instruction.kind != Value::Drop) continue;
                dischargeDrop(*block, i, (InstDrop&)instruction);
            }
        }
    }
};

}

void dischargeOwnership(OptContext& opt) {
    Discharge discharge { opt, opt.repr.target.family == TargetFamily::Managed };

    for(auto module: opt.program.modules) {
        opt.module = module;

        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];
            if(function->signature || function->blocks.isEmpty()) continue;

            discharge.run(*function);
        }
    }
}
