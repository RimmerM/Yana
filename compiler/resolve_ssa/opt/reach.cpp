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
 *    over it. `fun.cpp` looks it up the same way.
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
 *  - **every teardown**, and this is the one place the walk is deliberately blunter than the MIR
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
 * Globals are left alone entirely. `Global::reachable` is what `lowerGlobal` reads, nothing in this
 * directory removes a `global.addr`, and a sweep that answered about globals would have to be right
 * about the constant tree as well - which is a second question with a second way to be wrong for a
 * few bytes of read-only data.
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

    void root(DeclPtr<mir::Function> function) {
        if(!function) return;

        auto entry = seen.add(function.offset);
        if(entry.existed) return;

        *entry.value = true;
        reached.push(function);
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

                if(inst->kind == Inst::FunValue) root(((InstFunValue&)*inst).callee);
            }
        }
    }

    void run() {
        auto& program = opt.program;

        /*
         * The roots that are not reached from anywhere, in the order `markReachable` takes them.
         *
         * The allocator first, because it is the one that does not depend on which kind of
         * compilation this is: a program with a `@box` anywhere releases one through these three
         * whether it has an entry or is a library.
         */
        if(program.usesBoxes) {
            intrinsic::Known allocator[3] = { intrinsic::Known::allocateHeap,
                                              intrinsic::Known::freeHeap,
                                              intrinsic::Known::isHeapAddress };

            for(auto role: allocator) root(program.roles.function(role));
        }

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

        // And the teardowns - see the file comment for why this is a flag on the declaration rather
        // than the type walk `mir::summary.cpp` performs.
        for(auto modulePointer: program.modules) {
            auto module = opt.decl[modulePointer];

            for(auto pointer: module->functions.contents(opt.decl)) {
                if(opt.decl[pointer]->teardown) root(pointer);
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

} // namespace ssa
