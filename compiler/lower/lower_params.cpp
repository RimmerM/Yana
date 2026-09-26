#include "lower_params.h"
#include "lower_builder.h"
#include "lower_fold.h"

/*
 * See lower_params.h for which signatures may change and why the pass is a module pass. This file is
 * the three walks: which functions are fixed, which of the others' parameters go, and the rewrite of
 * both sides.
 */

namespace {

bool ownConvention(LowerCallType type) {
    return type == LowerCallType::Complex || type == LowerCallType::Simple;
}

struct ParameterRemover {
    LowerBase base;
    LowerModule& module;

    // The functions whose signature is somebody else's too, keyed by offset - see the header.
    HashMap<U32, U8> fixed;

    // Per function whose parameters change this round: where its flags start in `dropped`.
    HashMap<U32, U32> firstFlag;
    Array<bool> dropped;

    void fix(LowerPtr<LowerFunction> fun) {
        if(fun) fixed.add(fun.offset, 1);
    }

    void fixRelocations(LowerGlobal* global) {
        for(auto relocation: global->relocations.contents(base)) fix(relocation.function);
    }

    /*
     * Whether one reader of a `fun` is a call this pass can rewrite: the callee operand and no other,
     * under the function's own convention, with one operand per parameter. The arity is checked
     * because the validator checks it and a call that disagreed would be one this pass made worse.
     */
    bool rewritableUse(LowerInst* user, LowerValue* value, LowerFunction* target) {
        if(user->kind != LowerInst::Call) return false;
        if(((LowerInstCall*)user)->getCallType() != target->callType) return false;
        if(user->usedCount != target->args.size() + 1) return false;

        auto used = user->used();
        if(used.ptr[0] != value - base) return false;
        for(Size i = 1; i < used.length; i++) {
            if(used.ptr[i] == value - base) return false;
        }

        return true;
    }

    void collectFixed() {
        for(auto global: module.globalOrder) fixRelocations(base[global]);

        for(auto funPtr: module.functionOrder) {
            auto fun = base[funPtr];
            if(fun->prefix) fixRelocations(base[fun->prefix]);

            if(fun->foreignBoundary || !ownConvention(fun->callType) || fun->name == module.entry
               || funPtr == module.allocator) {
                fix(funPtr);
            }

            for(auto blockPtr: fun->blocks.contents(base)) {
                for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
                    auto inst = base[instPtr];
                    if(inst->kind != LowerInst::Fun) continue;

                    auto target = ((LowerInstFun*)inst)->target;
                    auto value = &((LowerInstSingle*)inst)->result;

                    for(auto userPtr: value->uses.contents(base)) {
                        if(!rewritableUse(base[userPtr], value, base[target])) {
                            fix(target);
                            break;
                        }
                    }
                }
            }
        }
    }

    /*
     * Which parameters go this round, decided for every function before any call is rewritten. A
     * function's call to itself passes its own parameters, so rewriting one can leave another
     * parameter unread mid-round - and a signature cut to a set its callers were not cut to is the
     * mismatch the validator names.
     */
    bool decide() {
        firstFlag.clear();
        dropped.clear();
        auto any = false;

        for(auto funPtr: module.functionOrder) {
            if(fixed.getValue(funPtr.offset).isJust()) continue;

            auto fun = base[funPtr];
            auto unused = false;
            for(auto arg: fun->args.contents(base)) {
                if(base[arg]->result.uses.size() == 0) unused = true;
            }

            if(!unused) continue;

            firstFlag.add(funPtr.offset, U32(dropped.size()));
            for(auto arg: fun->args.contents(base)) dropped.push(base[arg]->result.uses.size() == 0);
            any = true;
        }

        return any;
    }

    // Every call to a function `decide` cut, cut to match. Answers whether this body had one.
    bool rewriteCalls(LowerFunction& fun) {
        auto changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
                auto call = base[instPtr];
                auto callee = directCallee(base, call);
                if(!callee) continue;

                auto first = firstFlag.getValue((callee - base).offset);
                if(!first) continue;

                auto used = call->used();
                Size kept = 1;
                for(Size i = 1; i < used.length; i++) {
                    if(dropped[first.unwrap() + i - 1]) {
                        dropUse(base, used.ptr[i], instPtr);
                        continue;
                    }

                    used.ptr[kept++] = used.ptr[i];
                }

                call->usedCount = U16(kept);
                changed = true;
            }
        }

        return changed;
    }

    void rewriteSignature(LowerFunction& fun, U32 first) {
        for(Size i = fun.args.size(); i-- > 0;) {
            if(dropped[first + i]) fun.args.remove(base, i);
        }

        // The index is the argument's position, and the validator holds the two equal.
        for(Size i = 0; i < fun.args.size(); i++) base[fun.args.get(base, i)]->flags = U8(i);
    }

    void run() {
        collectFixed();

        // Bounded, because each round only removes: a parameter comes back never, so the number of
        // rounds is at most the depth of a chain of callers handing one word down, and four is past
        // every chain `test/base` has.
        for(Size round = 0; round < 4 && decide(); round++) {
            for(auto funPtr: module.functionOrder) {
                auto fun = base[funPtr];
                if(rewriteCalls(*fun)) removeDeadValues(base, module.arena, *fun);
            }

            for(auto funPtr: module.functionOrder) {
                auto first = firstFlag.getValue(funPtr.offset);
                if(first) rewriteSignature(*base[funPtr], first.unwrap());
            }
        }
    }
};

} // namespace

void removeUnusedParameters(LowerBase base, LowerModule& module) {
    ParameterRemover remover {base, module};
    remover.run();
}
