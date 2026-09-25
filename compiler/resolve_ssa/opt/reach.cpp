#include "pass.h"

/*
 * Which functions the program can still arrive at, asked again over SSA - README.md's section A row
 * for `markProgramReachable`, and section D's first missing piece.
 *
 * `mir::Function::reachable` is the interprocedural pass's answer, and it is the right answer until
 * something removes a reference. The inliner removes references: a call spliced into its caller is a
 * call that no longer names its callee, and a callee whose every site was spliced is a body nothing
 * can run. Leaving that stale is wrong in two directions and only one of them is harmless. It costs
 * *work* to optimize and lower a body nothing emits, which is a waste; and it feeds the sole-call-site
 * threshold, which is a **decision** - [[yana-escape-before-inlining]] item 5 is the shipping stage's
 * version of exactly that bug, where three sites became four because the fourth was in a function
 * that had already stopped existing, and a three-instruction teardown was refused by a budget that
 * had been reduced for a call site nothing could reach.
 *
 * So this runs at the top of every inliner round and once at the end, and it is the only thing in
 * this directory that writes a declaration.
 *
 * ## Why the walk is not `mir::summary.cpp`'s walk
 *
 * That one walks MIR, and it *is* the walk that specializes - it makes a specialization the moment a
 * call discovers one - so it cannot be re-run after `releaseMir()` even if the region were still
 * there. What it also does is fill `Summary`, mark environment slots and draw the invocation graph,
 * none of which changes here. What is left is the reachability half over the SSA a body now has.
 *
 * ## The three edges, and the four roots
 *
 * An edge is one of:
 *
 *  - **`InstCall::callee`**, where `through` is `Declaration`. Every other `through` names a slot or
 *    a value and reaches nothing from here, which is `markReachable`'s own ruling: what runs is what
 *    the *caller* of the erased body put in the slot, and that caller is what reached it.
 *  - **`InstCall::environment`** - the witness table a call to a body compiled once hands over. Each
 *    of its slots' `functions` is a declaration named by the table and by nothing else:
 *    `erase.cpp`'s `witnessFor` and `thunkFor` look each of them up in the lowered function map, so
 *    dropping one is a table pointing at a function that was never emitted.
 *  - **`InstFunValue::callee`** - a function named as a value, which the lowering turns into a thunk
 *    over it. `fun.cpp` looks it up the same way;
 *  - **a function constant in a reachable global** - a raw function pointer is a direct address, so
 *    there is no `InstFunValue` or call instruction to name its target. The initial global
 *    reachability answer is available before this final function sweep and supplies that edge.
 *
 * A root is one of:
 *
 *  - **`program.entry`**, where the program has one, and every non-generic function of the **root
 *    module** where it does not - the program/library split `markReachable` makes, restated rather
 *    than inferred. The entry is the one reference in the stage with **no diagnostic** behind it:
 *    `Lowering::run` sets `LowerModule::entry` from the function it happens to be lowering, so an
 *    entry that lost `reachable` is a module with no entry at all and nothing says so.
 *  - **the allocator**, where `Program::usesBoxes`. `allocateHeap`, `freeHeap` and `isHeapAddress`
 *    are called by the *lowering* - `lower/box.cpp` - so no instruction here names them and no edge
 *    could find them.
 *  - **every teardown, where the program has no entry or is JavaScript** - and, since J138, a
 *    native program roots a teardown only where a `drop`, a closure environment or a witness
 *    descriptor still left in the SSA can run it (`reachTeardown`, which follows the lowering's own
 *    halves so that `teardownMember` is asked nothing the summary walk did not already mint). A
 *    teardown whose last drop the optimizer erased or copied away is then not lowered - cause D of
 *    J94 §6.2, `LiteralFold.yana` 43 lowered instructions to 1. What follows is the reasoning for
 *    the blunt root the other two kinds of compilation keep; the note about `reachTeardown` is
 *    where the type walk's own terms are.
 *
 *    Blunt, and this is the one place the walk is deliberately blunter than the MIR
 *    one. `mir::summary.cpp`'s `reachTeardown` follows an `instance Drop(T)` from the type being
 *    dropped, recursing through fields, array elements and boxes; reproducing it here would be a
 *    second statement of a rule that has one, and calling the real one is worse than that - it goes
 *    through `teardownMember`, which *mints a specialization on a miss* by reading MIR, and the MIR
 *    region is gone. So a function whose declaration says it **is** a teardown (`Function::teardown`,
 *    set on the member of an `instance Drop(T)` or `Reclaim(T)` and copied onto every specialization
 *    of one) is a root. That is an exact superset of what `reachTeardown` can name - the derived
 *    walk emits generated lower functions and names no `mir::Function` at all - and what it costs is
 *    that a teardown whose last drop was optimized away stays lowered. It was reachable before this
 *    pass existed, so the cost is a size win not taken rather than a regression.
 *
 * The `GenSlotKind::Property` slot is the one edge neither walk draws, and it is the MIR walk's gap
 * rather than this one's: `erase.cpp`'s property accessor emits a teardown of the field type, and
 * `reachEnvironment` passes only a `Type` slot through `reachTeardown`. It is unreachable today
 * because the accessor is refused for anything that is not trivially copyable, and the teardown root
 * above covers it here in any case.
 *
 * ## What it may do to the answer
 *
 * **Clear, never set.** Everything this reaches was reachable before - the inliner adds no reference
 * that was not already in the program, since a clone of a callee's body holds the callee's own
 * references - so a function this walk finds and `reachable` disagreed about would be a defect in
 * one of the two walks. Asserted in an assertion build rather than repaired, because repairing it
 * would hide it.
 *
 * And only where the function has an `ssa` body. A declaration without one is a signature, an
 * intrinsic or a body the conversion declined; `Lowering::run` skips it on that test alone, so
 * clearing its flag would change nothing and lose the record of what MIR concluded.
 *
 * ## And the globals, which are a second walk and not this one
 *
 * `markReachableGlobals` is below, and it runs **once, after the per-function loop** rather than
 * inside the inliner's rounds. That is the whole of the difference between the two questions: a
 * function stops being reachable when the inliner splices its last call away, which is a fact the
 * inliner's own threshold then reads; a *global* stops being reachable when the folder answers its
 * last read, which does not happen until every body has been optimized. Asking early would answer
 * about a program that no longer exists by the time anything is lowered.
 *
 * This file used to say globals were "left alone entirely", on the grounds that a sweep would have
 * to be right about the constant tree as well. It does have to be, and that is the walk below: a
 * string global is bytes plus a relocation from the run that describes it, so a global reachable
 * only from another global's initializer is reachable. The cost of getting it wrong is not "a few
 * bytes of read-only data" but a relocation pointing at storage that was never emitted, which is
 * why the tree is walked rather than approximated.
 */

namespace ssa {

namespace {

struct Reachability {
    OptContext& opt;

    // The worklist *is* the answer - the functions found so far, drained from the front - so the two
    // cannot come to hold different sets. `mir::Interprocedural::reached` is the same arrangement.
    Array<DeclPtr<mir::Function>> reached;
    HashMap<U32, bool> seen;
    Size next = 0;

    /*
     * Whether a teardown is rooted only where something can still run it - J138 - rather than every
     * declaration that is one. Native only: the JavaScript backend builds its teardowns out of its
     * own walk (`js/teardown.cpp`), which this has not been checked against, and a library with no
     * entry keeps every one, since what drops its values is a program this build has not seen.
     */
    bool narrowTeardowns = false;

    void root(DeclPtr<mir::Function> function) {
        if(!function) return;

        auto entry = seen.add(function.offset);
        if(entry.existed) return;

        *entry.value = true;
        reached.push(function);

        // Under `-check-locations` the lowering calls the located twin wherever this body calls a
        // check role - see `run` below, where the reason the twin is rooted here is written.
        if(opt.context.settings.checkLocations) {
            auto& roles = opt.program.roles;
            if(function == roles.function(intrinsic::Known::checkFailed)) {
                root(roles.function(intrinsic::Known::checkFailedAt));
            } else if(function == roles.function(intrinsic::Known::checkCondition)) {
                root(roles.function(intrinsic::Known::checkConditionAt));
            }
        }
    }

    /*
     * The three allocator roles, rooted together and at most once.
     *
     * Together, because a build whose provider answers `isHeapAddress` releases boxes through it and
     * one whose provider does not has an unfilled role and roots nothing - `mir::summary.cpp` states
     * the same rule at the same three names. At most once because the walk asks per instruction now
     * and a program has many.
     */
    bool allocatorRooted = false;

    void rootAllocator() {
        if(allocatorRooted) return;
        allocatorRooted = true;

        intrinsic::Known allocator[3] = { intrinsic::Known::allocateHeap,
                                          intrinsic::Known::freeHeap,
                                          intrinsic::Known::isHeapAddress };

        for(auto role: allocator) root(opt.program.roles.function(role));
    }

    /*
     * Every function a witness table names - `erase.cpp`'s `witnessFor` and its `GenSlotKind::
     * Function` neighbour, read from the same list they build their thunks out of.
     *
     * `mir::summary.cpp`'s `reachEnvironment` is this loop plus a `reachTeardown` of every `Type`
     * slot. The second half is absent here on purpose and is not a second statement of the rule: the
     * teardown root above has already taken every function that walk could name.
     */
    void reachEnvironment(DeclPtr<mir::GenEnvironment> environment) {
        if(!environment) return;

        for(auto witness: opt.decl[environment]->slots.contents(opt.decl)) {
            for(auto member: witness.functions.contents(opt.decl)) root(member);
            if(!narrowTeardowns || !witness.type) continue;

            // A descriptor's teardown - `Lowering::teardownFor`'s halves for the placement - and a
            // property writer's teardown of the field it overwrites (`erase.cpp`'s `set(...)`).
            if(witness.kind == mir::GenSlotKind::Type) {
                auto ownership = mir::ownershipOf(opt.decl, witness.type);
                reachTeardown(witness.type, ownership.drop != mir::TeardownKind::None,
                              witness.storage != mir::StorageClass::Region &&
                              ownership.reclaim != mir::TeardownKind::None, LocationId());
            } else if(witness.kind == mir::GenSlotKind::Property &&
                      opt.decl[witness.type]->kind == mir::Type::Record) {
                auto& record = *(mir::RecordType*)opt.decl[witness.type];
                if(witness.field >= record.fields.size()) continue;

                auto declared = record.fields.get(opt.decl, witness.field);
                if(mir::tearsDownField(opt.decl, declared)) {
                    reachTeardown(declared.occurrence.type, true, true, LocationId());
                }
            }
        }
    }

    // A raw function pointer in a global is a constant address rather than an SSA function-value
    // instruction. Walk aggregate initializers so fixed callback tables root every entry they hold.
    void rootConstant(const mir::Constant& constant) {
        if(constant.kind == mir::Constant::Function) {
            root(constant.asFunction());
            return;
        }

        if(constant.kind != mir::Constant::Aggregate) return;
        for(auto part: opt.decl[constant.aggregate()]->values.contents(opt.decl)) {
            rootConstant(part);
        }
    }

    /*
     * The teardowns a `drop` of `type` runs, found from the type - J138, and cause D of
     * `doc/progress/2026-09-23-J94-legacy-comparison.md` §6.2. This is `Lowering::emitTeardown`'s
     * recursion restated over declarations rather than emitted: the authored member of each half
     * the walk runs, and the members' own halves where the half is derived - fields that
     * `tearsDownField`, a boxed field's target, a fixed array's element and a sum's alternatives.
     * A function value or an iterator ends it, because what runs there is the closure's own
     * teardown and `FunValue` roots that where the closure is built.
     *
     * It follows the lowering's halves exactly rather than a superset of them, because
     * `teardownMember` mints a specialization on a miss and the MIR it would mint from is gone: a
     * type the lowering asks about is one the summary walk already asked about, and a type only
     * a looser walk visited is not.
     */
    HashMap<U64, bool> tornDown;

    void reachTeardown(DeclPtr<mir::Type> type, bool runDrop, bool runReclaim, LocationId source) {
        if(!type || !runDrop && !runReclaim) return;

        auto key = (U64(type.offset) << 2) | (U64(runDrop) << 1) | U64(runReclaim);
        auto entry = tornDown.add(key);
        if(entry.existed) return;
        *entry.value = true;

        auto& decl = opt.decl;
        auto kind = decl[type]->kind;
        if(kind == mir::Type::Variable) return;

        auto ownership = mir::ownershipOf(decl, type);

        if(runDrop && ownership.drop == mir::TeardownKind::Authored) {
            root(mir::teardownMember(opt.context, opt.program, intrinsic::Known::drop, type, source));
        }

        if(runReclaim && ownership.reclaim == mir::TeardownKind::Authored) {
            root(mir::teardownMember(opt.context, opt.program, intrinsic::Known::reclaim, type,
                                     source));
        }

        auto childDrop = runDrop && ownership.drop == mir::TeardownKind::Derived;
        auto childReclaim = runReclaim && ownership.reclaim == mir::TeardownKind::Derived;
        if(!childDrop && !childReclaim) return;

        if(kind == mir::Type::Array) {
            auto& array = *(mir::ArrayType*)decl[type];
            if(!array.hasFixedLength() || !mir::hasTeardown(decl, array.element)) return;

            reachTeardown(array.element, childDrop, childReclaim, source);
            return;
        }

        if(kind != mir::Type::Record) return;

        // Every field of every alternative, which is `emitSumTeardown`'s arms and the single
        // constructor's walk alike; a boxed field's target is the field's own type.
        for(auto field: ((mir::RecordType*)decl[type])->fields.contents(decl)) {
            if(!mir::tearsDownField(decl, field)) continue;
            reachTeardown(field.occurrence.type, childDrop, childReclaim, source);
        }
    }

    void walk(mir::Function& function) {
        if(!function.ssa) return;

        auto body = opt.base[function.ssa];

        for(U32 i = 0; i < body->blocks.size(); i++) {
            auto block = body->block(opt.base, BlockId(i));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(opt.base)) {
                auto inst = opt.base[pointer];

                if(inst->kind == Inst::Call) {
                    auto& call = (InstCall&)*inst;
                    if(call.through == mir::CallThrough::Declaration) root(call.callee);
                    reachEnvironment(call.environment);
                    continue;
                }

                if(inst->kind == Inst::Drop) {
                    if(narrowTeardowns) {
                        reachTeardown(((InstDrop&)*inst).held.type, true, true, inst->source);
                    }

                    continue;
                }

                if(inst->kind == Inst::FunValue) {
                    auto callee = ((InstFunValue&)*inst).callee;
                    root(callee);

                    // `closureBlockTeardown`, which the lowering builds for the environment of a
                    // closure that leaves its frame and runs both halves of.
                    if(narrowTeardowns && callee && opt.decl[callee]->environment) {
                        reachTeardown(opt.decl[callee]->environment, true, true, inst->source);
                    }

                    /*
                     * And the block a closure that leaves its frame carries -
                     * `Lowering::emitClosureBlock`, which asks exactly this of exactly this
                     * declaration and then calls `allocateHeap`. The same shape as the run below.
                     */
                    if(callee && opt.decl[callee]->environmentStorage != mir::StorageClass::Frame) {
                        rootAllocator();
                    }
                }

                /*
                 * A placement ownership analysis sent off the frame, which `lower/inst.cpp`'s
                 * `heapStorage` turns into `allocateHeap` and whose `free` turns into `freeHeap` -
                 * the third of the three sites, and the second this walk can see one at a time.
                 *
                 * `Program::usesHeapPlacement` is what the MIR walk roots on, at every request,
                 * because it runs before this optimizer and has to keep the bodies alive to be
                 * optimized against. This is the later, narrower question, and it is worth asking:
                 * a binary that carries the allocator carries `allocateHeap` and `initHeap` and
                 * `growHeap` behind it, and over `test/resolve` asking it here rather than there was
                 * **-1,776 bytes**.
                 */
                if(inst->kind == Inst::Alloc &&
                   ((InstAlloc&)*inst).storage != mir::StorageClass::Frame) {
                    rootAllocator();
                }

                if(inst->kind == Inst::Free) rootAllocator();

                /*
                 * A run request the placement pass left on the heap, which is the one call this
                 * walk cannot see: `lower/inst.cpp` expands the instruction into `allocateHeap`,
                 * one tier below here, exactly as the located check pair is expanded below the
                 * roots above.
                 *
                 * Asked per instruction rather than rooted with the allocator at the top, and that
                 * is the whole point of asking it here: `place.cpp` has already run, so a program
                 * whose every literal ended up in the frame names the allocator nowhere and does
                 * not carry it. `Program::usesRuns` is set at every request because the *MIR*
                 * reachability walk runs before the placement and has to keep the body alive to be
                 * placed against; this is the later, narrower question.
                 */
                if(inst->kind == Inst::RunSlots && !((InstRunSlots&)*inst).placed) rootAllocator();
            }
        }
    }

    void run() {
        auto& program = opt.program;

        /*
         * The roots that are not reached from anywhere, in the order `markReachable` takes them.
         *
         * The allocator first, because it is the one that does not depend on which kind of
         * compilation this is: a program with a **boxed field** anywhere builds and releases one
         * through these three whether it has an entry or is a library, and no instruction names it -
         * the lowering builds the call out of the record's shape. That is the half `Program::
         * usesBoxes` still answers, and `decl.h` is where the split is argued; the other half is a
         * placement, which `walk` above roots one instruction at a time.
         */
        if(program.usesBoxes) rootAllocator();

        /*
         * The located check pair under `-check-locations` is rooted by `root` itself, each where its
         * unlocated twin is reached. The substitution happens in the lowering
         * (`Lowering::locatedCheck`), after this walk, so nothing in the SSA names either located
         * declaration - but every check call is built in MIR, so the walk does find the call the
         * lowering will substitute. It used to root both whatever the program held, which kept the
         * whole reporting path in a build none of whose checks could fail (J168).
         */

        narrowTeardowns = program.entry && !isJsMode(opt.context.settings);

        if(program.entry) {
            root(program.entry);
        } else {
            // A library has no start, so its root module's declarations are the roots. A generic is
            // never one: its parameters are unbound, so it has no layout and nothing to lower.
            auto module = opt.decl[program.root()];

            for(auto pointer: module->functions.contents(opt.decl)) {
                if(mir::isGeneric(opt.decl, opt.decl[pointer]->gen)) continue;
                root(pointer);
            }
        }

        // And the teardowns - every one where the walk is not narrowed, which the file comment
        // argues; where it is, the drops, closures and descriptors the walk finds root them.
        if(!narrowTeardowns) {
            for(auto modulePointer: program.modules) {
                auto module = opt.decl[modulePointer];

                for(auto pointer: module->functions.contents(opt.decl)) {
                    if(opt.decl[pointer]->teardown) root(pointer);
                }
            }
        }

        // A raw function pointer in a reachable global has no SSA value naming the function. Root
        // those constants before draining the ordinary call/value edges. The global flag is the
        // same pre-optimization reachability answer this pass already relies on when it clears the
        // functions it did not find.
        for(auto modulePointer: program.modules) {
            auto module = opt.decl[modulePointer];
            for(auto pointer: module->globals.contents(opt.decl)) {
                auto global = opt.decl[pointer];
                if(global->reachable) rootConstant(global->initial);
            }
        }

        while(next < reached.size()) walk(*opt.decl[reached[next++]]);

        for(auto modulePointer: program.modules) {
            auto module = opt.decl[modulePointer];

            for(auto pointer: module->functions.contents(opt.decl)) {
                auto function = opt.decl[pointer];
                if(!function->ssa) continue;

                auto found = seen.getValue(pointer.offset);
                if(found) {
                    // Clear, never set - the file comment's rule, checked rather than repaired.
                    assertTrue(function->reachable);
                    continue;
                }

                function->reachable = false;
            }
        }
    }
};

} // namespace

void markProgramReachable(OptContext& opt) {
    Reachability walk { opt };
    walk.run();
}

namespace {

/*
 * The globals a reachable body still names, and the ones those name in turn.
 *
 * The reason this is worth a walk: `lib/`'s scalar constants - `pageBytes`, `vectorBytes`,
 * `heapRegionSize`, `runFromHeap` and six more - are immutable module-level `let`s whose *reads*
 * fold to the number, in this stage as in the shipping one. What did not go away was the storage,
 * so every program that touched the heap carried ten `.data` objects each referenced exactly once,
 * by its own definition. §78.7's item 7 and 1584 bytes over the thirteen-program corpus.
 *
 * Two kinds of edge, and both are needed:
 *
 *  - a **`GlobalAddress`** in a surviving instruction, which is every read, every write and every
 *    `addressOf`. A global reference in this IR has no other spelling;
 *  - a **`Constant::Address`** anywhere in a rooted global's initializer or in a body's own
 *    constants, which is the string case: `stringLiteral` describes a run out of the address of a
 *    byte global, so the bytes are named by the descriptor's initializer and by nothing else. Three
 *    instructions hold a constant, not one - `Const`, and a `Map`'s table and its fallback. A walk
 *    that missed the table asserted in `Lowering::materialize` on the first constructor map of
 *    strings, which is what says the third edge is real rather than defensive.
 *
 * `Global::dynamic` needs no root of its own. What fills such a global is a `store` through a
 * `global.addr` in the entry's own body, so the first edge finds it; a dynamic global nothing writes
 * and nothing reads is one whose initializer ran nowhere.
 *
 * The supplied roles need none either, and that is the same answer the shipping stage gives:
 * `Lowering::declareGlobal` is what fills `LowerModule::builtins`, so a global this walk drops is
 * one the entry stub then stores nothing into - which is what `commandLineCount` unread has always
 * meant.
 *
 * **Clear, never set**, for `Reachability`'s reason: MIR marked every global a body mentioned, and
 * this is that answer minus what the optimizer removed.
 */
struct GlobalReachability {
    OptContext& opt;

    Array<DeclPtr<mir::Global>> reached;
    HashMap<U32, bool> seen;
    Size next = 0;

    void root(DeclPtr<mir::Global> global) {
        if(!global) return;

        auto entry = seen.add(global.offset);
        if(entry.existed) return;

        *entry.value = true;
        reached.push(global);
    }

    // A constant and everything under it - the aggregate case is a fixed array's elements or a
    // constructor's fields, and a string's descriptor is exactly that shape with an address in it.
    void rootConstant(const mir::Constant& constant) {
        if(constant.kind == mir::Constant::Address) {
            root(constant.asGlobal());
            return;
        }

        if(constant.kind != mir::Constant::Aggregate) return;

        for(auto part: opt.decl[constant.aggregate()]->values.contents(opt.decl)) {
            rootConstant(part);
        }
    }

    void walk(mir::Function& function) {
        if(!function.ssa) return;

        auto body = opt.base[function.ssa];

        for(U32 i = 0; i < body->blocks.size(); i++) {
            auto block = body->block(opt.base, BlockId(i));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(opt.base)) {
                auto inst = opt.base[pointer];

                if(inst->kind == Inst::GlobalAddress) {
                    root(((InstGlobalAddress&)*inst).global);
                } else if(inst->kind == Inst::Const) {
                    rootConstant(((InstConst&)*inst).constant);
                } else if(inst->kind == Inst::Map) {
                    // The third holder of a constant, and the one a walk over `Const` alone misses:
                    // a `match` over literals carries its whole table, so a constructor map of
                    // strings names every one of their byte globals and nothing else does.
                    auto& map = (InstMap&)*inst;
                    for(auto entry: map.entries.contents(opt.base)) rootConstant(entry.value);
                    rootConstant(map.fallback);
                }
            }
        }
    }

    void run() {
        auto& program = opt.program;

        for(auto modulePointer: program.modules) {
            auto module = opt.decl[modulePointer];

            for(auto pointer: module->functions.contents(opt.decl)) {
                auto function = opt.decl[pointer];
                if(function->reachable) walk(*function);
            }
        }

        // The tree, after the bodies: a global reached from a body may name others in its own
        // initializer, and those may name more.
        while(next < reached.size()) rootConstant(opt.decl[reached[next++]]->initial);

        for(auto modulePointer: program.modules) {
            auto module = opt.decl[modulePointer];

            for(auto pointer: module->globals.contents(opt.decl)) {
                auto global = opt.decl[pointer];

                if(seen.getValue(pointer.offset)) {
                    // Clear, never set - the same rule the function walk above checks, and the same
                    // way: a global this pass reached that the stage before it had already dropped
                    // is a reference minted here, which nothing may do.
                    assertTrue(global->reachable);
                    continue;
                }

                global->reachable = false;
            }
        }
    }
};

} // namespace

void markReachableGlobals(OptContext& opt) {
    GlobalReachability walk { opt };
    walk.run();
}

} // namespace ssa
