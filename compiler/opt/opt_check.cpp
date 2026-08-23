#include "opt_pass.h"

/*
 * The bounds and divisor checks, taken out again for a build that does not want them -
 * Analysis-Modules.md Move 4.
 *
 * ## Why this exists at all
 *
 * `settings.checks` used to be answered during *resolution*: `ExprResolver::checksEnabled` asked it,
 * and a build with the checks off emitted no call and computed no condition. That made a resolved
 * program a function of the setting, so an image had to carry it in its key - and, worse than the
 * key, it meant a library compiled with the checks on could not be linked into a program compiled
 * without them, because the two are not the same IR. Emitting the call unconditionally and removing
 * it here is what makes them the same IR.
 *
 * ## Why it is here and not in `lower`
 *
 * There is no `lower` on the JavaScript path: `js::genProgram` consumes the resolve IR directly. So
 * "the target expands it" has exactly one place that both targets pass through, and this stage is
 * it - `optimizeProgram` is called once per backend, with that backend's `ReprTarget`, from
 * `lowerProgram` and from `genProgram`.
 *
 * ## Why it runs before the `-no-opt` return
 *
 * Because `-no-checks` has to stay *free* rather than cheap. If this were an ordinary pass, a build
 * with the optimizer off would emit a length load, a comparison and a call at every subscript and
 * then keep all three. So it runs unconditionally, and it does its own dead-value sweep rather than
 * leaving one to `eliminateDeadValues`, which does not run either.
 *
 * The sweep is deliberately narrow: only values that became unread *because a check was removed*,
 * and only pure ones. An unoptimized build is full of instructions nothing reads, and removing those
 * is not this pass's business - it would make `-no-checks -no-opt` a third shape of output rather
 * than the second one with the checks taken out.
 */

namespace {

// The operands of an erased instruction, as candidates for the sweep. Recorded before the erase,
// since dropping the uses is what makes them unread and the operand list is gone afterwards.
void collectOperands(OptContext& opt, Value& instruction, Array<ModulePtr<Value>>& into) {
    eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
        if(operand) into.push(operand);
    });
}

bool removeChecks(OptContext& opt, Function& function) {
    Array<ModulePtr<Value>> pending;
    auto removed = false;

    for(auto blockPointer: function.blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(Size i = block->instructionCount(); i-- > 0;) {
            auto pointer = block->instructionAt(opt.local, i);
            auto instruction = opt.local[pointer];

            if(instruction->kind != Value::Call) continue;
            if(!isCheckCall(opt, ((InstCall&)*instruction).callee)) continue;

            // A check returns nothing, so nothing reads it and the erase is unconditional. The
            // assertion inside `eraseInstruction` is what would say so if that ever stopped holding.
            collectOperands(opt, *instruction, pending);
            opt.ir().eraseInstruction(pointer);
            removed = true;
        }
    }

    if(!removed) return false;

    /*
     * And the condition behind each one, transitively.
     *
     * A worklist rather than a walk of the function, because what may have become dead is exactly
     * what the removed calls named and what *those* named in turn - the length load and the compare
     * of a bounds test, and nothing else. Walking the function would find every other unread pure
     * value in an unoptimized body as well.
     */
    while(pending.size()) {
        auto pointer = pending[pending.size() - 1];
        pending.pop();

        auto value = opt.local[pointer];
        if(value->kind == Value::Arg || value->useCount() != 0 || !isPureValue(*value)) continue;

        collectOperands(opt, *value, pending);
        opt.ir().eraseInstruction((ModulePtr<Inst>)pointer);
    }

    return true;
}

}

bool dischargeChecks(OptContext& opt) {
    if(opt.context.settings.checks || !opt.program.checkCondition) return false;

    auto removed = false;

    for(auto module: opt.program.modules) {
        opt.module = module;

        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];
            if(function->signature || function->blocks.isEmpty()) continue;

            opt.function = function;
            removed = removeChecks(opt, *function) || removed;
        }
    }

    opt.function = nullptr;
    opt.module = nullptr;
    return removed;
}
