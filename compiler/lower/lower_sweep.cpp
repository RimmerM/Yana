#include "lower_sweep.h"

/*
 * See lower_sweep.h for the roots and the edges. One worklist over both kinds of declaration, since
 * each kind reaches the other: a body names a table, and the table's slots name bodies.
 */

namespace {

struct Sweep {
    LowerBase base;
    LowerModule& module;

    HashMap<U32, bool> seenFunctions;
    HashMap<U32, bool> seenGlobals;
    Array<LowerPtr<LowerFunction>> functions;
    Array<LowerPtr<LowerGlobal>> globals;

    void reach(LowerPtr<LowerFunction> fun) {
        if(!fun) return;
        auto entry = seenFunctions.add(fun.offset);
        if(entry.existed) return;
        *entry.value = true;
        functions.push(fun);
    }

    void reach(LowerPtr<LowerGlobal> global) {
        if(!global) return;
        auto entry = seenGlobals.add(global.offset);
        if(entry.existed) return;
        *entry.value = true;
        globals.push(global);
    }

    void walkGlobal(LowerGlobal& global) {
        for(auto relocation: global.relocations.contents(base)) {
            reach(relocation.function);
            reach(relocation.global);
            reach(relocation.anchor);
        }
    }

    void walkFunction(LowerFunction& fun) {
        reach(fun.prefix);

        for(auto blockPtr: fun.blocks.contents(base)) {
            for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
                auto inst = base[instPtr];
                if(inst->kind == LowerInst::Fun) reach(((LowerInstFun*)inst)->target);
                else if(inst->kind == LowerInst::Global) reach(((LowerInstGlobal*)inst)->target);
            }
        }
    }

    void run() {
        if(!module.entry) return;
        auto entry = module.functions.get(module.entry);
        if(!entry) return;

        reach(entry.unwrap());
        for(auto funPtr: module.functionOrder) {
            if(base[funPtr]->foreignBoundary) reach(funPtr);
        }

        reach(module.imageAnchor);
        for(auto builtin: module.builtins) reach(builtin);

        Size nextFunction = 0, nextGlobal = 0;
        while(nextFunction < functions.size() || nextGlobal < globals.size()) {
            while(nextFunction < functions.size()) walkFunction(*base[functions[nextFunction++]]);
            while(nextGlobal < globals.size()) walkGlobal(*base[globals[nextGlobal++]]);
        }

        // Compacted in place, so that what is left keeps the order it was declared in - which is the
        // order it is emitted in (see LowerModule::functionOrder).
        Size kept = 0;
        for(Size i = 0; i < module.functionOrder.size(); i++) {
            auto funPtr = module.functionOrder[i];
            if(seenFunctions.getValue(funPtr.offset)) {
                module.functionOrder[kept++] = funPtr;
            } else {
                module.functions.remove(base[funPtr]->name);
                if(module.allocator == funPtr) module.allocator = nullptr;
            }
        }
        while(module.functionOrder.size() > kept) module.functionOrder.pop();

        kept = 0;
        for(Size i = 0; i < module.globalOrder.size(); i++) {
            auto globalPtr = module.globalOrder[i];
            if(seenGlobals.getValue(globalPtr.offset)) {
                module.globalOrder[kept++] = globalPtr;
            } else {
                module.globals.remove(base[globalPtr]->name);
            }
        }
        while(module.globalOrder.size() > kept) module.globalOrder.pop();
    }
};

} // namespace

void removeUnreachableDeclarations(LowerBase base, LowerModule& module) {
    Sweep { base, module }.run();
}
