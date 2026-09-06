#include "pass.h"

/*
 * One walk, five rewrites, driven by the use lists - README.md's section B.
 *
 * The shipping stage runs constant folding, the algebraic identities, the `Bool` bitwise rewrite,
 * CSE and dead-value removal as five separate passes and repeats the group up to eight times,
 * because its IR has no use-list worklist and each of the five could only hear about the others'
 * work by being walked again. Every one of the five is local to an instruction and its operands, and
 * `ssa::Value` already carries `uses`, so here they are one pass: fold an operand and its readers go
 * back on the queue, erase an instruction and its operands' definers do.
 *
 * The five stay in separate functions and the fold table stays in a file of its own, because the
 * table is large and is not a walk. What is here is the walk, the queue, and the one thing all five
 * have in common - the ownership rule below.
 *
 * ## The ownership rule, which is what makes this the safe tier to do any of it in
 *
 * Every rewrite here either **erases** an instruction or **unifies** two values, and the linear rule
 * is what decides which instructions may be either. `isRemovable` is the whole of it:
 *
 *  - only a **`kSsaPure`** kind, which is the column that says an instruction may be recomputed or
 *    not computed at all;
 *  - **no operand that is tracked** - `Owned` or `Affine` - and no operand named by a `Consume` use.
 *    Erasing an instruction drops every use it records, so a consumption that went away would leave
 *    an owner unconsumed on a path that had consumed it, which is the *verifier's* first rule and
 *    would be reported at this pass's checkpoint. No pure kind declares `kSsaConsumes` today, so this
 *    guard fires on nothing; it is written because the two columns are independent and a kind added
 *    to both would arrive here silently;
 *  - results that are **`Trivial` or `Address`** and nothing else. A `Guaranteed` value is the case
 *    this declines rather than reasons about: what a borrow is checked against is its *loan*, and
 *    unifying two borrows on different loans moves an endpoint past a use with nothing in the IR
 *    changed to say so. `Editor::replaceAllUses` refuses it, and refusing it here is refusing it
 *    before the assertion rather than at it.
 *
 * A `Const` is never duplicated by anything here and could not be: `Editor::addConst` asserts its
 * type is not linear, so a constant's result is `Trivial` by construction and there is no owner for
 * a second copy of one to be a second copy *of*.
 *
 * ## And the thing a fold must not make
 *
 * A `{}` is **not a value at this tier**. `Conversion::convertNode` drops a unit constant on the
 * floor and `addCall` takes a `hasResult` flag for the same reason - a value of no bits is one
 * nothing below this can map - so a fold that answered a unit would be inventing the one thing the
 * conversion is careful never to produce. Nothing here can: every constant `fold.cpp` makes is an
 * integer, a float or a constructor index, and every identity answers with an operand that was
 * already standing there.
 */

namespace ssa {

namespace {

struct Simplifier {
    OptContext& opt;

    /*
     * The queue, and the bit that keeps an instruction from being on it twice.
     *
     * A head index rather than a stack, so that the seeding walk's order survives into the first
     * pass over the body: a fold reaches an operand before its reader, which is what makes a chain of
     * arithmetic collapse in one sweep rather than in one sweep per link.
     *
     * Both are indexed by `InstId`, which is dense and **grows** while this runs - every constant a
     * fold makes is a new instruction - so `marked` is pushed into rather than sized once.
     */
    Array<U32> queue;
    Array<U8> marked;
    Size head = 0;

    void mark(Size index, U8 value) {
        while(marked.size() <= index) marked.push(0);
        marked[index] = value;
    }

    void push(Inst* inst) {
        if(!inst || !inst->block) return;

        auto index = Size(inst->id.index);
        if(index < marked.size() && marked[index]) return;

        mark(index, 1);
        queue.push(U32(index));
    }

    void push(SsaPtr<Inst> pointer) {
        if(pointer) push(opt.base[pointer]);
    }

    // Everything that reads one value, which is what a rewrite of the instruction defining it has
    // just changed the operands of.
    void pushUsers(Value& value) {
        for(auto user: value.uses(opt.base)) push(user);
    }

    // And the other direction: what this instruction read, whose last reader an erase may have been.
    void pushOperands(Inst& inst) {
        forEachUse(opt.base, inst, [&](Use used) {
            if(used.isSet()) push(opt.base[used.value()]->definer);
        });
    }

    /*
     * Whether this instruction may be taken out or stood in for - the ownership rule, see the file
     * comment, which is where each of the three clauses is argued.
     */
    bool isRemovable(Inst& inst) {
        if(!isPure(inst.kind)) return false;

        auto ok = true;
        forEachUse(opt.base, inst, [&](Use used) {
            if(!used.isSet()) { ok = false; return; }
            if(used.kind() == UseKind::Consume) ok = false;
            if(isTracked(opt.base[used.value()]->convention)) ok = false;
        });

        for(auto resultPointer: inst.results.contents(opt.base)) {
            auto convention = opt.base[resultPointer]->convention;
            if(convention != Convention::Trivial && convention != Convention::Address) ok = false;
        }

        return ok;
    }

    // Whether one value may stand for another. The conventions have to *agree* rather than merely
    // both be untracked: replacing an address by a scalar is not a substitution however the linear
    // rule reads it, and `Guaranteed` is declined above before this is asked.
    bool isUnifiable(Value& from, Value& to) {
        return from.convention == to.convention &&
               (from.convention == Convention::Trivial || from.convention == Convention::Address);
    }

    Value* soleResult(Inst& inst) {
        if(inst.results.size() != 1) return nullptr;
        return opt.base[inst.results.get(opt.base, 0)];
    }

    // The readers of one value pointed at another and the instruction taken out, in that order -
    // `eraseInst` asserts that nothing reads what it removes.
    void replaceAndErase(Inst& inst, Value& result, Value& with) {
        pushUsers(result);

        auto editor = opt.ir();
        editor.replaceAllUses(result, with);
        editor.eraseInst(inst);

        pushOperands(inst);
        push(with.definer);
    }

    /*
     * The constants and the algebraic identities - `opt/fold.cpp`, which is the table.
     *
     * Answers a value and never a rewrite, so the whole of what is here is the ownership guard and
     * the erase. A fold that made a constant left it standing immediately above this instruction,
     * which is where `Editor::setInsertBefore` put it and is what keeps the replacement dominating
     * every reader of what it replaces.
     */
    bool tryFold(Inst& inst) {
        if(!isRemovable(inst)) return false;

        auto result = soleResult(inst);
        if(!result) return false;

        auto folded = foldInst(opt, inst);
        if(!folded || folded == result || !isUnifiable(*result, *folded)) return false;

        replaceAndErase(inst, *result, *folded);
        return true;
    }

    /*
     * A `Bool` select is a boolean function of two truth values, and every one of those is bitwise.
     *
     * `a && b` reaches this tier as a diamond whose join is a block argument; if-conversion (phase 7)
     * turns that into `select c ? b : False`, and what a backend then makes of it is a conditional
     * move with the constant arm loaded into a register first - three instructions and ten bytes for
     * one `and`. The two arms of a `Bool` select are 0 or 1 by the type's own definition, so a select
     * between them is `and`, `or` or `not` of values the function already has, and all three are
     * `Bitwise(Bool)` members that `lower/inst.cpp` already lowers.
     *
     * **Here rather than in the folder because every rule needs an instruction that does not exist
     * yet.** That is the division between the two files: `c ? True : False` is `c`, a value already
     * standing there, so it is `foldSelect`'s and is answered before this is reached.
     *
     * **What produces a `Bool` select is `convertSelects`, and nothing else.** The only other
     * `select` in the corpus is the vector one - `Lanewise.select`, and `min`/`max` expanded out of
     * it - whose condition is a mask and whose result is never a `Bool`, which is what the type test
     * below is for. So this was written in phase 4 and fired on nothing until phase 7 arrived; it now
     * fires seventeen times over the corpus, and `IfConvert.yana` is the fixture that says all four
     * rules are right. Whichever `select` reaches here carries `Inspect` operands, which is
     * `select.cpp`'s doing and is what lets `isRemovable` look at one at all.
     *
     * The **mirror pair** - a false arm of `True`, a true arm of `False` - is declined, and it is the
     * shipping stage's reason: each is the same rewrite over `!c`, which is a second instruction this
     * would have to insert, and whether that is free is a question about the rest of the function
     * rather than about this instruction. It is free where the program already computed `!c` and
     * costs a `not` where it did not, and nothing here can see which.
     *
     * **De Morgan is not here either.** `!a || !b` becoming `!(a && b)` is one instruction fewer and
     * puts the complement where a branch can absorb it, which is real - but the branch that absorbs
     * it is `negatedBranchCondition` in `lower/lower_fold.cpp`, one tier down, and the shape it reads
     * arrives there whichever way this stage spells it. Worth doing when there is a measurement
     * asking for it.
     *
     * The condition is checked against `Bool` as well as the result, because `Select` is also the
     * lane-wise one: a vector select's condition is a `Mask`, its arms are vectors, and neither the
     * arms being 0 or 1 nor `and` meaning what it means here is true of one.
     */
    bool tryBoolean(Inst& inst) {
        if(inst.kind != Inst::Select || !isRemovable(inst)) return false;

        auto& select = (InstSelect&)inst;
        auto result = soleResult(inst);
        if(!result) return false;

        auto boolean = mir::booleanType(opt.program);
        if(result->type() != boolean) return false;

        auto condition = opt.base[select.condition.value()];
        if(condition->type() != boolean) return false;

        auto whenTrue = constantValueOf(opt, opt.base[select.whenTrue.value()]);
        auto whenFalse = constantValueOf(opt, opt.base[select.whenFalse.value()]);

        auto editor = opt.ir();
        editor.setInsertBefore(inst);

        Inst* made = nullptr;

        if(whenTrue && whenFalse && whenTrue.unwrap() == 0 && whenFalse.unwrap() == 1) {
            // `!c`. One `not`, which is `Bitwise(Bool).not` and is what `op.def` says a complement at
            // a `Bool` is - the complement of the *value*, which `lower/inst.cpp` emits as one `xor`
            // where a `Bool`'s shape is known.
            made = editor.addUnary(inst.source, UnaryOp::Not, editor.use(*condition, UseKind::Inspect),
                                   result->occurrence, Convention::Trivial);
        } else if(whenFalse && whenFalse.unwrap() == 0) {
            // `c && b` answers `b` where `c` held and `False` where it did not, which is `c & b`.
            // Neither arm has to be a constant on the other side - what makes the rewrite sound is
            // that both arms are `Bool`, so both are already 0 or 1.
            made = editor.addBinary(inst.source, BinaryOp::And,
                                    editor.use(*condition, UseKind::Inspect), select.whenTrue,
                                    result->occurrence, Convention::Trivial);
        } else if(whenTrue && whenTrue.unwrap() == 1) {
            made = editor.addBinary(inst.source, BinaryOp::Or,
                                    editor.use(*condition, UseKind::Inspect), select.whenFalse,
                                    result->occurrence, Convention::Trivial);
        }

        if(!made) return false;

        replaceAndErase(inst, *result, *editor.resultOf(*made));
        push(made);
        return true;
    }

    /*
     * Whether two instructions carry the same thing beside their operands - the shipping stage's
     * `sameExtra`, as one switch.
     *
     * **The default declines**, and that is the whole design of it: a kind this has never been
     * looked at for is one whose immediates might be anything, so it is left alone rather than
     * unified on its operands alone. What that costs is a removal; what the other default would cost
     * is two `field.addr`s at different fields becoming one.
     */
    bool sameImmediates(Inst& a, Inst& b) {
        switch(a.kind) {
            case Inst::Const:
                return ((InstConst&)a).constant == ((InstConst&)b).constant;
            case Inst::Binary:
                return ((InstBinary&)a).op == ((InstBinary&)b).op;
            case Inst::Unary:
                return ((InstUnary&)a).op == ((InstUnary&)b).op;
            case Inst::Select:
                return true;
            case Inst::Fma:
                return true;
            case Inst::GlobalAddress:
                return ((InstGlobalAddress&)a).global == ((InstGlobalAddress&)b).global;
            case Inst::FieldAddress: {
                auto& left = (InstFieldAddress&)a;
                auto& right = (InstFieldAddress&)b;
                return left.record == right.record && left.field == right.field &&
                       left.shape == right.shape && left.property == right.property;
            }
            case Inst::ElementAddress:
                return ((InstElementAddress&)a).container == ((InstElementAddress&)b).container;
            case Inst::Metric: {
                auto& left = (InstMetric&)a;
                auto& right = (InstMetric&)b;
                return left.metric == right.metric && left.of == right.of;
            }
            default:
                return false;
        }
    }

    /*
     * Whether two pure instructions compute the same thing.
     *
     * Operands are compared as SSA values, which makes this an equality rather than a congruence:
     * two instructions with equal-but-distinct operands are left alone, and the queue catches them
     * once the operands themselves have been unified. The **use kind** is compared with the value,
     * because a borrow and an inspection of one value are two different statements about it.
     *
     * The results have to agree on their whole occurrence and on their convention, not only on the
     * logical type: an owned `String` and a shared window into one are the same type and different
     * representations, and `Value::occurrence` is the field that says so.
     */
    bool sameComputation(Inst& a, Inst& b) {
        if(a.kind != b.kind || !sameImmediates(a, b)) return false;
        if(a.results.size() != b.results.size()) return false;

        for(Size i = 0; i < a.results.size(); i++) {
            auto left = opt.base[a.results.get(opt.base, i)];
            auto right = opt.base[b.results.get(opt.base, i)];

            if(left->occurrence != right->occurrence) return false;
            if(left->convention != right->convention) return false;
        }

        SmallArray<Use, 4> left, right;
        forEachUse(opt.base, a, [&](Use used) { left.push(used); });
        forEachUse(opt.base, b, [&](Use used) { right.push(used); });

        if(left.size() != right.size()) return false;

        for(Size i = 0; i < left.size(); i++) {
            if(left[i].packed != right[i].packed) return false;
        }

        return true;
    }

    /*
     * A computation over nothing but constants, which is worth *more* than removing it would save.
     *
     * The target spells one of these where it is used rather than computing it - an immediate operand
     * - so giving it a second reader is what stops that: the value then has to exist somewhere both
     * readers can see, which is a register. Unifying two of them turns two free things into one that
     * costs. The shipping stage found this with `null`, where unifying two pointer zeroes took a
     * record literal apart into a literal and two assignments to it.
     *
     * An instruction with **no operands at all** is the same rule stated at the leaf, and this stage
     * reads it the other way round from the shipping stage's. That one keeps `sizeof` and a symbol's
     * address out of the exemption - "constants the target supplies rather than ones written here" -
     * and here the corpus says otherwise: `global.addr` is such an instruction, `lower/inst.cpp`
     * folds a **single-reader** one straight into the access it addresses, and unifying the two in
     * `log = log + 1` gave it two readers and took the address out into a value of its own. Fourteen
     * lines across `lens/Iterator.yana` alone, for one instruction removed. So the exemption is
     * "materialized where it is used", which covers `const`, `global.addr`, `metric`, `gen.value` and
     * `fun.value` in one clause rather than by a list of kinds.
     */
    bool materializedAtUse(Inst& inst) {
        Size operands = 0;
        Size constants = 0;

        forEachUse(opt.base, inst, [&](Use used) {
            operands++;

            auto definer = used.isSet() ? opt.base[used.value()]->definer : nullptr;
            if(definer && opt.base[definer]->kind == Inst::Const) constants++;
        });

        return operands == constants;
    }

    /*
     * Block-local pure CSE: the second of two identical computations answered by the first.
     *
     * **Block-local, deliberately.** `eliminateCommonValues` in `lower/lower_cse.cpp` is the one that
     * walks the dominator tree, it runs over this stage's output through `optimizeLowerFunction`, and
     * a second dominator-tree CSE here would be the same answer computed twice - README.md's section
     * A rules on it. What is left for this tier is the case that walk cannot reach *cheaply enough to
     * be worth a second analysis*: two spellings of one expression side by side, which is what the
     * folds above and the inliner below produce.
     *
     * Within one block, "the first one dominates the second" is the instruction order and needs no
     * tree at all - which is the whole reason this fits inside a worklist pass.
     */
    bool tryCommonSubexpression(Inst& inst) {
        if(!isRemovable(inst) || materializedAtUse(inst)) return false;

        auto result = soleResult(inst);
        if(!result) return false;

        auto block = opt.base[inst.block];

        for(auto pointer: block->instructions(opt.base)) {
            auto earlier = opt.base[pointer];
            if(earlier == &inst) break;

            if(!sameComputation(*earlier, inst)) continue;

            auto with = opt.base[earlier->results.get(opt.base, 0)];
            if(!isUnifiable(*result, *with)) continue;

            replaceAndErase(inst, *result, *with);
            return true;
        }

        return false;
    }

    // A pure instruction whose results nothing reads. The last of the five and the one the other four
    // leave work for: a fold points every reader somewhere else, and what it leaves behind is this.
    bool tryDead(Inst& inst) {
        if(!isRemovable(inst)) return false;

        for(auto resultPointer: inst.results.contents(opt.base)) {
            if(opt.base[resultPointer]->useCount() != 0) return false;
        }

        pushOperands(inst);
        opt.ir().eraseInst(inst);
        return true;
    }

    void visit(Inst& inst) {
        // An instruction an earlier rewrite already took out. The table entry stands - `Body::insts`
        // is indexed by `InstId` - so what says it is gone is that it is in no block.
        if(!inst.block) return;

        if(tryFold(inst)) return;
        if(tryBoolean(inst)) return;
        if(tryCommonSubexpression(inst)) return;
        tryDead(inst);
    }

    void run() {
        for(auto blockPointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[blockPointer];
            if(block->deleted) continue;

            for(auto pointer: block->instructions(opt.base)) push(opt.base[pointer]);
        }

        while(head < queue.size()) {
            auto index = queue[head++];
            mark(index, 0);

            visit(*opt.base[opt.body->insts.get(opt.base, index)]);
        }
    }
};

} // namespace

void simplify(OptContext& opt) {
    Simplifier simplifier { opt };
    simplifier.run();
}

} // namespace ssa
