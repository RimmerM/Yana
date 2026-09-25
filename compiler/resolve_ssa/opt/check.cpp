#include "pass.h"

/*
 * The library's checks, taken back out for a build that does not want them - `-no-checks`, and
 * `resolve_ssa/opt/README.md`'s phase 12.
 *
 * ## What a check *is* here, which is not what it was in the shipping stage
 *
 * In `compiler/resolve` (the shipping stage until J173 deleted it) the compiler emitted
 * `checkCondition(index >= length)` at every subscript and held a pointer to that function, so
 * `dischargeChecks` was a walk looking for a call to it. This stage emits one and only one: a `[T
 * *n]` element is a projection of the container's own place rather than a call, so there is no
 * accessor body for its bound to live in and `checkFixedBound` in `resolve_mir/resolve_expr.cpp`
 * reaches for the same `checkCondition` role. Every *other* check is the library's own -
 * `lib/Core/Array.yana`'s `Index.get` writes
 *
 *     if (bitcast(index) :: USize) >= (bitcast(self.length) :: USize) then checkFailed()
 *
 * in the library body and `@inline(always)` grafts it into the caller during SSA conversion. So what
 * arrives either way is an ordinary comparison, an ordinary branch, and an arm holding one call -
 * the compiler's own is that shape too, one inlining later.
 *
 * That moves the meaning of the switch and it is worth being exact about it. `-no-checks` here is
 * **not** "the checks the compiler inserted", of which there is one; it is "a branch whose whole
 * taken arm is a call to the `checkFailed` role, removed". `lib/Core/Array.yana`'s `reserve`
 * writes `if !resize(self.run, wide) then checkFailed()` and that is the same shape, so a
 * `-no-checks` build drops the allocation-failure stop along with the bounds tests. That is the
 * honest reading of a library whose checks *are* library code and not a hole in the rule: what the
 * switch turns off is `checkFailed`, wherever the library reached for it.
 *
 * ## The rule is structural, deliberately
 *
 * A branch qualifies when one of its arms is a block that holds **exactly** a `call` to the
 * `checkFailed` role, through `Declaration`, with no arguments, followed by a `jump`. Anything else
 * is left standing - `if cond: log(); checkFailed()` keeps both, and so does an arm that returns
 * rather than falling through, because the graft's `then` form has no `else` and `checkFailed`
 * returns, so falling through to the join is the shape a check actually has and anything else was
 * written by a program with something else in mind.
 *
 * The alternative was to recognize the *condition* - an unsigned `ge` of two `bitcast`s - and it is
 * worse in both directions: it would miss the check the day the library spells the test differently,
 * and it would fire on a program that happened to compare two indices in front of an abort. The
 * callee is the fact; the arithmetic in front of it is not.
 *
 * ## Why it runs where it runs
 *
 * Above the `optimizeIr` gate, which is the deleted `opt_check.cpp`'s ruling carried over unchanged
 * and is the whole of what keeps `-no-checks` **free** rather than cheap. An ordinary pass would
 * leave a build with the optimizer off computing a length, a comparison and a branch at every
 * subscript and then keeping all three. So this runs unconditionally, and it does its own two
 * sweeps rather than leaving them to passes that do not run.
 *
 * The two other rulings that file makes carry over as they stand: the resolved IR is the same with
 * the checks on or off - nothing above this reads `settings.checks` - so a library built checked
 * links into an unchecked program; and the removal is in the optimizer rather than in resolution or
 * lowering, because `js::genProgram` never lowers and `optimizeProgram` is the one place both
 * targets pass through.
 *
 * ## The two sweeps, and why the first one is narrow
 *
 * **The condition**, by a worklist seeded from the erased branch's condition and nothing else: the
 * `ge`, the two `bitcast`s, the `length` load and the `field.addr` under it, each removed only where
 * it is now unread and only where `isRemovableIfUnread` admits it. Not a general dead-value sweep -
 * an unoptimized body is full of instructions nothing reads, and removing those would make
 * `-no-checks -no-ir-opt` a third shape of output instead of the second one with the checks taken
 * out.
 *
 * **The arm**, by `removeUnreachableBlocks` - `cleanCfg`'s own sweep, called standalone. In an
 * optimized build `cleanCfg` would reach it a moment later anyway; in a `-no-checks -no-ir-opt`
 * build nothing else runs at all, and leaving a dead block holding a `call` would be exactly the
 * cheap-rather-than-free outcome the placement above is for. One implementation, reached two ways,
 * rather than a second reachability walk here.
 *
 * `checkFailed` itself is **not** made a root of `markProgramReachable`, and that is the point: once
 * the calls are gone the body should go too, and in an optimized build it does. An unoptimized build
 * still emits it, exactly as it still emits every other unreachable body, because reachability is
 * not a pass that runs there.
 */

namespace ssa {

namespace {

struct CheckDischarger {
    OptContext& opt;
    DeclPtr<mir::Function> checkFailed;

    /*
     * A block that is one call to `checkFailed` and a way out - the whole of the rule.
     *
     * No block arguments, because a check arm receives nothing: the graft branches on a value the
     * caller already has and the arm computes nothing to hand anywhere. Two instructions exactly, so
     * that a `log()` beside the stop keeps the branch. And the call compared against the
     * *declaration* the role names rather than against a name, which is what the role exists for.
     */
    bool isCheckArm(Block& block) {
        if(block.deleted || block.argumentCount() || block.instructionCount() != 2) return false;

        auto call = opt.base[block.instructionAt(opt.base, 0)];
        if(call->kind != Inst::Call) return false;

        auto& written = (InstCall&)*call;
        if(written.through != mir::CallThrough::Declaration) return false;
        if(written.callee != checkFailed || written.arguments.size()) return false;

        return opt.base[block.instructionAt(opt.base, 1)]->kind == Inst::Jump;
    }

    /*
     * The condition the branch left behind, removed as far up its chain as it is now unread.
     *
     * A worklist over the operands of what went, which was the shipping stage's shape and its
     * reason: what may have become dead is exactly what the branch named and what *those* named in
     * turn, and walking the body would find every other unread pure value in an unoptimized one as
     * well.
     *
     * `isRemovableIfUnread` is the ownership rule the rest of this directory uses with one kind more
     * admitted - `opt/cse.cpp` - so a borrow or a consumption stops the chain here exactly as it
     * stops `simplify`, and the `self.length` read does not. That read is the one instruction that
     * would otherwise survive into a `-no-checks -no-ir-opt` build, since the two passes that
     * ordinarily take a dead load are both below the gate.
     */
    void sweepCondition(Inst* from) {
        SmallArray<Inst*, 16> pending;
        if(from) pending.push(from);

        while(pending.size()) {
            auto inst = pending[pending.size() - 1];
            pending.pop();

            // An instruction an earlier round already took out, or one this chain reached twice -
            // `Body::insts` keeps the table entry, so what says it is gone is that it is in no block.
            if(!inst->block || !isRemovableIfUnread(opt, *inst)) continue;

            auto unread = true;
            for(auto resultPointer: inst->results.contents(opt.base)) {
                if(opt.base[resultPointer]->useCount()) unread = false;
            }

            if(!unread) continue;

            // The operands read out before the erase, since dropping the uses is what makes them
            // unread and the operand list is gone afterwards. A block argument has no definer -
            // `Value::definer` is null for one - and is where a chain stops rather than something to
            // follow: what stands behind it is one operand per incoming edge.
            forEachUse(opt.base, *inst, [&](Use used) {
                if(!used.isSet()) return;
                if(auto definer = opt.base[used.value()]->definer) pending.push(opt.base[definer]);
            });

            opt.ir().eraseInst(*inst);
        }
    }

    /*
     * One branch, folded to a jump at its other arm.
     *
     * Through `replaceTerminator` and the multiset rule, exactly as `cleanCfg`'s `foldBranch` does:
     * the surviving edge is in both the old successor list and the new one, so it keeps its
     * `incoming` entry and whatever operands it was carrying, and only the check arm loses one.
     *
     * The region rule is the file comment of `opt/cfg.cpp`: a rewrite that removes an edge is
     * confined to one region. The graft is straight-line and both arms are in the branch's own
     * region, so this is a guard rather than a case - what it declines is a shape no conversion
     * produces, and declining is cheaper than an argument a later kind of region has to be
     * re-checked against.
     */
    bool discharge(Block& block) {
        auto last = block.terminator(opt.base);
        if(!last || last->kind != Inst::Branch) return false;

        auto& branch = (InstBranch&)*last;
        if(!branch.whenTrue.target || !branch.whenFalse.target) return false;
        if(branch.whenTrue.target == branch.whenFalse.target) return false;

        auto whenTrue = opt.base[branch.whenTrue.target];
        auto whenFalse = opt.base[branch.whenFalse.target];

        Edge* kept = nullptr;
        if(isCheckArm(*whenTrue) && whenTrue->region == block.region) {
            kept = &branch.whenFalse;
        } else if(isCheckArm(*whenFalse) && whenFalse->region == block.region) {
            kept = &branch.whenTrue;
        }

        if(!kept) return false;
        if(opt.base[kept->target]->region != block.region) return false;

        // Held before the terminator goes, because that is what drops the branch's use of it and
        // therefore what may leave it unread.
        Inst* condition = nullptr;
        if(branch.condition.isSet()) {
            if(auto definer = opt.base[branch.condition.value()]->definer) {
                condition = opt.base[definer];
            }
        }

        SmallArray<Use, 4> arguments;
        for(auto used: kept->arguments.contents(opt.base)) arguments.push(used);

        auto target = opt.base[kept->target];
        auto editor = opt.ir();
        editor.replaceTerminator(block, editor.makeJump(last->source, *target, arguments.pointer(),
                                                        arguments.size()));

        sweepCondition(condition);
        return true;
    }

    bool run() {
        auto removed = false;

        for(auto pointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[pointer];
            if(block->deleted) continue;

            removed = discharge(*block) || removed;
        }

        if(removed) removeUnreachableBlocks(opt);
        return removed;
    }
};

} // namespace

/*
 * Program-wide, and a `bool` where every other pass here is a `void`.
 *
 * Both because of where it runs. It is called above the `optimizeIr` gate, so the per-function walk
 * the driver performs below has not started and this owns its own; and what it answers is whether
 * the driver owes a `verifyProgram` on a path that otherwise runs no pass at all - see
 * `optimizeProgram`, where a `-no-ir-opt` build that discharged nothing returns the program the
 * conversion already verified.
 */
bool dischargeChecks(OptContext& opt) {
    if(opt.context.settings.checks) return false;

    auto checkFailed = opt.program.roles.function(intrinsic::Known::checkFailed);
    if(!checkFailed) return false;

    auto discharged = false;

    for(auto modulePointer: opt.program.modules) {
        auto module = opt.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(opt.decl)) {
            auto function = opt.decl[functionPointer];
            if(!function->reachable || !function->ssa) continue;

            opt.function = function;
            opt.body = opt.base[function->ssa];

            CheckDischarger discharger { opt, checkFailed };
            if(!discharger.run()) continue;

            discharged = true;
            checkpoint(opt, "after dischargeChecks"_v);
        }
    }

    opt.function = nullptr;
    opt.body = nullptr;
    return discharged;
}

} // namespace ssa
