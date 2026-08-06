/*
 * Reachability: which functions a finished program can arrive at.
 *
 * The only pass in this group that runs over the IR rather than over syntax, and the only one whose
 * question is about the program as a whole. What makes it more than a call graph walk is that a
 * value can name a function without calling it - a closure's table, a witness entry, a global's
 * initializer - so a place is a reason to keep something too.
 */

#include "module_internal.h"
#include "analyze.h"
#include "const.h"
#include "core.h"
#include "expr.h"
#include "generic.h"
#include "host.h"
#include "index.h"
#include "name.h"
#include "native.h"
#include "verify.h"
#include "witness.h"
#include "../parse/ast.h"

/*
 * What the program actually contains.
 *
 * `used` is set on a function as each call to it is resolved, which answers "was this called" and
 * not "can this run". The two differ as soon as a module's functions call each other: Native's
 * allocateHeap calls heapClassOf whether or not any program allocates, so every one of them would
 * be marked and an unimported runtime would be emitted into every binary.
 *
 * The answer is rebuilt here as a closure over what the root module can reach, once every body
 * exists. Globals come along the same way - a global is part of the program exactly when
 * something that runs reads or writes it.
 */
static void markPlace(ModuleBase local, const Place& place) {
    if(place.root == PlaceRoot::Global && place.global) local[place.global]->used = true;
}

/*
 * `tables` is the compiler-built constants reached so far, and walking them is not optional: a table
 * holds addresses, and an address is a reason for what it names to exist. A TypeDesc naming the glue
 * that tears its type down is the only thing keeping that glue alive, and dropping the relocation
 * instead would leave a slot the emitted code calls through holding zero.
 */
/*
 * A closure header no path leads to any more - see markClosureHeaders in compiler/opt.
 *
 * Kept out of the walk rather than merely out of the emitted output, because what a table holds is
 * the point: the entry functions in a header are reached from that header and from nowhere else, so
 * a dead one that is still walked keeps a `dropAt$` alive that nothing can call.
 */
static bool isDeadClosureHeader(ModuleBase local, ModulePtr<Global> table) {
    auto lambda = local[table]->prefixOf;
    return lambda && !local[lambda]->closureHeaderRead;
}

static void markReachable(Program& program, Array<ModulePtr<Function>>& pending,
                          Array<ModulePtr<Global>>& tables) {
    ModuleBase local = *program.arena;

    auto reachFunction = [&](ModulePtr<Function> callee) {
        if(!callee || local[callee]->used) return;

        local[callee]->used = true;
        pending.push(callee);
    };

    auto reachTable = [&](ModulePtr<Global> table) {
        if(!table || local[table]->used) return;
        if(isDeadClosureHeader(local, table)) return;

        local[table]->used = true;
        tables.push(table);
    };

    while(pending.isNotEmpty() || tables.isNotEmpty()) {
        while(tables.isNotEmpty()) {
            auto table = local[tables.pop().unwrap()];

            for(auto slot: table->table.contents(local)) {
                reachFunction(slot.function);
                reachTable(slot.global);
            }
        }

        if(pending.isEmpty()) continue;
        auto function = local[pending.pop().unwrap()];
        auto& reach = reachFunction;

        /*
         * A lifted lambda's closure header, which nothing in the IR names.
         *
         * It is attached to the function rather than referred to by it - `Function::closureHeader`,
         * emitted in front of the entry point natively and as `$h` on JS - so no instruction the
         * walk below can see mentions it. That went unnoticed while every root-module table was
         * seeded as reached; now that they are not, this is the only edge that keeps a live header's
         * entry functions alive, and without it the table is emitted naming functions that are not.
         */
        if(function->closureHeader && function->closureHeaderRead) reachTable(function->closureHeader);

        for(auto blockPointer: function->blocks.contents(local)) {
            for(auto instructionPointer: local[blockPointer]->instructions(local)) {
                auto& instruction = *local[instructionPointer];

                // A global is part of the program exactly when something that runs names storage
                // rooted in it, and which places an instruction names is one list - see
                // instructionPlaces. What the switch below is about is everything else an
                // instruction can reach: a callee, a table, a teardown.
                eachPlace(instruction, [&](const Place& place) { markPlace(local, place); });

                switch(instruction.kind) {
                    case Value::Call:
                        reach(((InstCall&)instruction).callee);
                        break;
                    case Value::GenCall:
                        reach(((InstGenCall&)instruction).callee);
                        reachTable(((InstGenCall&)instruction).env);

                        for(auto fill: ((InstGenCall&)instruction).fill.contents(local)) {
                            reachTable(fill.constant);
                        }

                        break;
                    case Value::Symbol:
                        // The address of a function or of a table, taken as a value: a function
                        // value's code word, and the environment descriptor it carries.
                        reach(((InstSymbol&)instruction).callee);
                        reachTable(((InstSymbol&)instruction).global);
                        break;
                    case Value::Move:
                        reach(((InstMove&)instruction).sink);
                        break;
                    case Value::Copy:
                        reach(((InstCopy&)instruction).copy);
                        break;
                    case Value::Drop:
                        // Both teardown implementations are reached from here and from nowhere
                        // else: a derived glue function has no call site in the source at all, and
                        // an authored instance may have none either. The same goes for the release
                        // of heap storage, which lowering emits as a call nothing in the IR names.
                        reach(((InstDrop&)instruction).drop);
                        reach(((InstDrop&)instruction).reclaim);
                        if(((InstDrop&)instruction).releaseStorage) reach(program.freeHeap);
                        break;
                    case Value::Alloc:
                        if(((InstAlloc&)instruction).storage == StorageClass::Heap) {
                            reach(program.allocateHeap);
                        }

                        break;
                    default:
                        break;
                }
            }
        }
    }
}

void markProgramReachable(Program& program) {
    ModuleBase local = *program.arena;
    Array<ModulePtr<Function>> pending;

    Array<ModulePtr<Global>> tables;

    /*
     * What the walk starts from, and there are two answers because there are two things a
     * compilation can be producing.
     *
     * **A program has one root: where it starts.** Everything a program can arrive at, it arrives at
     * from there - through a call, a table slot, a teardown - so ordinary reachability is the whole
     * answer and it applies to the root module exactly as it applies to every other. A function of
     * the root module that nothing can reach is dead code, and a `pub` on it will not change that:
     * `pub` says who may *name* it, which is a question about compiling against this module rather
     * than about running it.
     *
     * **A library has no start**, so its declarations are the roots: something outside this
     * compilation is going to call them, and there is nothing here that could say which. That is
     * today's rule, kept for exactly the case it was right for.
     *
     * Both conditions, and each rules out a different mistake. A library compile of a module that
     * happens to declare `main` is still a library, and rooting the walk there would emit one
     * function and drop everything the library exists to offer. A program whose root module declares
     * neither `main` nor a top-level statement has nowhere to start, and is reported as that by
     * whichever backend was asked for one rather than being silently emptied here.
     */
    if(program.entry && isExecutableMode(program.context.settings.mode)) {
        for(auto module: program.modules) {
            for(auto function: module->functionOrder.contents(local)) local[function]->used = false;
            for(auto global_: module->globalOrder.contents(local)) local[global_]->used = false;
        }

        local[program.entry]->used = true;
        pending.push(program.entry);

        markReachable(program, pending, tables);
        return;
    }

    for(auto module: program.modules) {
        /*
         * A named function of the root module is part of the library whether or not this compilation
         * can see a call to it; everything else has to be reached.
         *
         * Anonymous is the right test rather than a proxy for one: `addAnonymousFunction` is
         * documented as "reachable through something other than its name", so a function that is one
         * has, by construction, a reference somewhere for this walk to find - a call, a table slot,
         * an `InstDrop` half. If there is none, nothing can ever run it. What that keeps out is every
         * compiler-built function generated *into* the root module, which is most of them: glue is
         * built in the module that asked for it, so a program's own teardowns, entry thunks and
         * descriptors all land here. `reclaim$Step` with an empty body and no caller is what
         * including them looks like.
         */
        for(auto function: module->functionOrder.contents(local)) {
            local[function]->used = module->root && !local[function]->anonymous;
            if(local[function]->used) pending.push(function);
        }

        // The root module's tables are seeded alongside its functions rather than being taken as
        // already-reached, because what a table *holds* is the point: marking one used without
        // walking it would keep the bytes and drop everything their relocations name.
        // The same split the functions above get, and for the same reason: a *declared* global of
        // the root module is a root of the walk, and every compiler-built table is a finding.
        for(auto global_: module->globalOrder.contents(local)) {
            local[global_]->used = module->root && !local[global_]->anonymous;
            if(local[global_]->used) tables.push(global_);
        }
    }

    markReachable(program, pending, tables);
}
