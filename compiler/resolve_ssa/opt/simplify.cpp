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
 * is what decides which instructions may be either. `isRemovable` is the whole of it - defined in
 * `opt/cse.cpp`, because the walk of the dominator tree there asks the same question and two
 * spellings of it is one of them learning about a new convention and the other not:
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
     * The three questions all five rewrites ask - `opt/cse.cpp`, where each is defined and where the
     * walk of the dominator tree asks the same three.
     *
     * Wrappers rather than calls at every site, because the context is the only argument they add
     * and this file asks them a dozen times. `isRemovable` is the ownership rule and the file
     * comment above is where its three clauses are argued; `isUnifiable` is whether one value may
     * stand for another; `soleResult` is the one result a substitution needs.
     */
    bool isRemovable(Inst& inst) { return ssa::isRemovable(opt, inst); }
    bool isUnifiable(Value& from, Value& to) { return ssa::isUnifiable(from, to); }
    Value* soleResult(Inst& inst) { return ssa::soleResult(opt, inst); }

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

    // Whether two pure instructions compute the same thing, and whether one is spelled at its use
    // rather than computed - `opt/cse.cpp`, which is where both are defined and argued. The
    // block-local CSE below and the walk of the dominator tree there are one rewrite at two scopes,
    // so they are one statement of what "the same" means.
    bool sameComputation(Inst& a, Inst& b) { return ssa::sameComputation(opt, a, b); }
    bool materializedAtUse(Inst& inst) { return ssa::materializedAtUse(opt, inst); }

    /*
     * Block-local pure CSE: the second of two identical computations answered by the first.
     *
     * **Block-local, deliberately.** `cse` in `opt/cse.cpp` is the one that walks the dominator tree,
     * and this is not that pass with a smaller scope: it is the case a worklist over uses reaches for
     * free, because within one block "the first one dominates the second" is the instruction order.
     * What the folds above and the inliner below produce is two spellings of one expression standing
     * side by side, and a queue that is already running answers those without a tree or a scope
     * stack. The two share one statement of what "the same computation" is - `sameComputation`,
     * `isRemovable`, `isUnifiable` and `materializedAtUse`, declared in `pass.h`.
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
