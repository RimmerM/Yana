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
 * `Drop` is, into the calls it stands for. `Swap` and `Exchange` are, into the relocations and
 * writes `resolve/lower.cpp` already expanded them into - the code moves up, and moving it up is
 * what lets a body containing one be inlined at all.
 *
 * `Move` deliberately is **not**: it is the relocation itself rather than a decision about one -
 * `codegen/js` emits nothing for it but the name, and reads its kind to alias where a load would
 * deep-clone - so there is no lower form to turn it into. It is admitted to `clonableKind` instead,
 * which is also what lets the two expansions above be written in terms of it rather than past it.
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
 *     a non-generic body contains no `Drop`, `Swap` or `Exchange`
 *
 * and that is the statement `clonableKind` needs. The same bound applies to all three and for the
 * same reason - each is asked of the *place's* type rather than of the function, because a generic
 * body swaps concrete things too.
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

    /*
     * A relocation out of a place, as the `InstMove` it is.
     *
     * `Move` is not discharged and does not need to be - it is the lower form already, and J
     * admitted it to `clonableKind` for that reason. So the two expansions below are written *in
     * terms of* it rather than past it, which is also what keeps them one line each: whatever
     * relocating this type costs, a move is where that cost is already written down, sink and all.
     */
    /*
     * Whether a relocation of this type is expressible here at all.
     *
     * Two shapes are not, and both are cases `resolve/lower.cpp` handles with machinery that only
     * exists one stage down:
     *
     *  - a type this target made **one number**. `InstMove` of a memory type means "the address of
     *    the source", and a scalarized record has no address - lowering reads and writes it as bits
     *    through `materializeScalar` and `narrowRefAccess` instead. Discharging one produced a move
     *    of an address that was never there, and the backend followed it;
     *  - an **opaque** type, which has no layout to relocate.
     *
     * Declining is free: what is left is the instruction that was already there, lowered exactly as
     * it is today. What is lost is the inlining of a body containing one, which is the thing this
     * pass buys and not something it owes.
     */
    bool relocatable(TypePtr content) {
        if(!content || isGeneric(opt.global, content)) return false;

        auto& repr = opt.repr.of(content);
        return !repr.opaque && repr.scalarBits == 0;
    }

    ModulePtr<Value> relocate(Block& block, InstList& into, LocationId source, TypePtr content,
                              const Place& from, ModulePtr<Function> sink) {
        auto move = createInst<InstMove>(*opt.module, *opt.function, block, source, 0, content, from);
        move->sink = sink;
        if(sink) opt.local[sink]->used = true;

        into.push(move);
        return (ModulePtr<Value>)(move - opt.local);
    }

    void write(Block& block, InstList& into, LocationId source, const Place& to,
               ModulePtr<Value> value, Value::Kind kind) {
        into.push(createInst<InstInit>(*opt.module, *opt.function, block, source, 0,
                                       opt.program.scalar.unit, to, value, kind));
    }

    /*
     * `swap(a, b)` - three relocations through a temporary.
     *
     * The temporary is not removable and the reason is the operation: neither place may be written
     * until both have been read, so something has to hold the first while the second is written over
     * it. `resolve/lower.cpp` says exactly this and does exactly this; what moves up here is the
     * shape, not the reasoning.
     *
     * The writes are `Assign` into `a` and `b` and `Init` into the temporary, which is the ordinary
     * reading of both: the two places held values and the temporary is fresh. Neither owes a drop by
     * now - the drop pass has been and gone, and a move is what emptied each place before it was
     * written.
     */
    bool dischargeSwap(Block& block, Size index, InstSwap& swap) {
        auto content = swap.content;
        if(!relocatable(content)) return false;

        auto source = swap.source;
        InstList replacement;

        auto temporary = createInst<InstAlloc>(*opt.module, *opt.function, block, source, 0, content,
                                               maxLimit<U32>);
        auto storage = (ModulePtr<Value>)(temporary - opt.local);
        temporary->local = opt.function->addLocal(*opt.module, content, 0, storage);
        replacement.push(temporary);

        auto held = Place::inLocal(temporary->local);

        write(block, replacement, source, held,
              relocate(block, replacement, source, content, swap.a, swap.sink), Value::Init);
        write(block, replacement, source, swap.a,
              relocate(block, replacement, source, content, swap.b, swap.sink), Value::Assign);
        write(block, replacement, source, swap.b,
              relocate(block, replacement, source, content, held, swap.sink), Value::Assign);

        insertInstructions(opt, block, index, replacement);
        eraseInstruction(opt, (ModulePtr<Inst>)(&swap - opt.local));

        opt.changed = true;
        return true;
    }

    /*
     * `exchange(place, value)` - one relocation out and one write in, with no temporary.
     *
     * What is coming in is already a value rather than a place, so there is nothing to save from
     * being written over. A scalar result comes out in a register, so there the read is a plain load
     * and there is no relocation to perform at all; a memory-typed one needs storage, and gets a
     * fresh allocation exactly as the swap's temporary does.
     *
     * The slot is the resolver's own - `InstExchange::local` - and what changes is only which
     * instruction owns it. That matters: a slot's `Local::value` has to be the instruction that
     * *made* the storage, because lowering materializes a local through it. Pointing it at a load
     * of the same slot asks lowering for a value it has not lowered yet, and leaving it pointing at
     * the erased exchange leaves a slot naming an instruction that is no longer in any block. An
     * `InstAlloc` over the same slot is neither, and `setLocalValue` is the one door that writes
     * both halves of the pairing.
     */
    bool dischargeExchange(Block& block, Size index, InstExchange& exchange) {
        auto content = exchange.type;
        if(!relocatable(content)) return false;

        auto source = exchange.source;
        auto pointer = (ModulePtr<Inst>)(&exchange - opt.local);
        InstList replacement;
        ModulePtr<Value> old = nullptr;

        if(exchange.local == maxLimit<U32>) {
            auto loaded = createInst<InstLoadPlace>(*opt.module, *opt.function, block, source,
                                                    exchange.name, content, exchange.place);
            replacement.push(loaded);
            old = (ModulePtr<Value>)(loaded - opt.local);
        } else {
            auto allocation = createInst<InstAlloc>(*opt.module, *opt.function, block, source,
                                                    exchange.name, content, exchange.local);
            old = (ModulePtr<Value>)(allocation - opt.local);
            replacement.push(allocation);
            opt.function->setLocalValue(opt.local, exchange.local, old);

            write(block, replacement, source, Place::inLocal(exchange.local),
                  relocate(block, replacement, source, content, exchange.place, exchange.sink),
                  Value::Init);
        }

        // After the read, which is the whole of what the temporary in a swap exists to avoid needing.
        write(block, replacement, source, exchange.place, exchange.value, Value::Assign);

        insertInstructions(opt, block, index, replacement);
        replaceValue(opt, (ModulePtr<Value>)pointer, old);
        eraseInstruction(opt, pointer);

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

                switch(instruction.kind) {
                    case Value::Drop:
                        dischargeDrop(*block, i, (InstDrop&)instruction);
                        break;
                    case Value::Swap:
                        dischargeSwap(*block, i, (InstSwap&)instruction);
                        break;
                    case Value::Exchange:
                        dischargeExchange(*block, i, (InstExchange&)instruction);
                        break;
                    default:
                        break;
                }
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
