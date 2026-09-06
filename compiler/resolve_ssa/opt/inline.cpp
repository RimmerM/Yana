#include "pass.h"
#include "../dominance.h"

/*
 * Inlining: a call replaced by a copy of what it would have done - README.md's section A row for
 * `inlineCalls`, and the only pass here with a cost model.
 *
 * What it buys is mostly not the call. A callee copied into its caller is a body the *other* passes
 * can then see through: `simplify` folds it against the literals the site passed, `forwardMemory`
 * answers the loads of a parameter slot from the store that filled it, `removeDeadStorage` takes the
 * slot, and `cleanCfg` folds the branch a constant condition decided. That is why this runs
 * program-wide *before* the per-function loop rather than inside it - what it leaves behind is work
 * for the seven passes after it.
 *
 * ## The two shapes of copy
 *
 * A **straight-line** callee - one reachable block ending in `return` - is spliced into the caller's
 * own block in front of the call. No block is made, no edge moves, no block argument is declared,
 * and every reader of the call's result is pointed straight at the value the `return` named. That is
 * the common case and it is what keeps `.ssa.opt.expect` readable; it also keeps the callee's
 * `alloc` in the caller's own block, where the block-local half of `simplify` and the per-block scan
 * in `forwardMemory` can still see it.
 *
 * A callee with **control flow** needs the general form: the caller's block is cut in two at the
 * call, the callee's blocks are cloned into the gap, a `join` block is made carrying the result as
 * its one argument, and every cloned `return` becomes a jump to it. The join is a block of its own
 * rather than the second half of the split, because `Editor::addBlockArgument` refuses a block that
 * already holds instructions - a block argument holds on entry and is not a position among them -
 * and `cleanCfg` folds the pair back together immediately afterwards, since the join's one
 * successor has one predecessor.
 *
 * ## What the clone has to map, and it is five things rather than two
 *
 * **Values.** A callee value maps to its clone; a callee **entry-block argument** maps to the call's
 * operand at that position, which is `resolve_ssa/README.md`'s "parameters arrive as entry-block
 * arguments and become storage" read from the caller's side. Every use keeps its `UseKind`: a
 * consuming use of a parameter is a consuming use of the argument, and the call instruction that was
 * consuming it is erased in the same rewrite, so the value is consumed exactly once either way.
 *
 * **Blocks**, with fresh `BlockId`s, cloned in **reverse postorder**. Not in table order: an operand
 * names a value defined in a block that *dominates* its own, and block ids are creation order, so a
 * table-order walk can reach a use before the definition it maps to. Reverse postorder is the order
 * that cannot - which is the same order `Lowering::lowerBody` takes and for the same reason. A block
 * the callee cannot reach is not cloned at all: it is on no path, and its instructions owe nothing.
 *
 * **Loans**, re-minted in the caller's table. A `LoanId` is dense per body, so carrying one across
 * is the mistake §8.8 refused to make with `mir::LoanId`. A loan whose storage set names the
 * callee's own allocations is re-minted naming the clones of them, which is exact. A loan with an
 * **empty** set is the *caller's* loan seen from inside the callee - a borrowed parameter arrives
 * already borrowed - and it maps to the caller's own empty loan of the same capability rather than
 * to the loan the borrowing argument carries. That is the permissive answer and it is the only one
 * that works:
 *
 *  - the empty loan is not one lender. `Editor::addResult` gives it to *every* borrow the conversion
 *    did not refine, so it is the callee's "rooted somewhere outside" rather than "rooted in
 *    parameter 2"; a callee with two borrowed parameters has one of them, and picking either is
 *    picking wrong for the other's borrows. Rooting it in the union of the borrowing arguments'
 *    lenders is sound as a may-set and *over*-reports: the loan then ends when any of them is
 *    released, and a `->` argument handed over in front of the call has already released one;
 *  - and `Editor::replaceAllUses` admits a borrow replaced by one "on the same loan or on the
 *    caller's", which is exactly what the continuation handover needs when a callee returns a
 *    borrow. An empty loan is admitted at that substitution and a body-local one has to match
 *    whatever the caller's own `attachLoan` decided, which nothing here can arrange.
 *
 * What it costs is stated precisely rather than left as "the endpoint rule inside the copy", because
 * the case that reaches it is not the one that sentence describes. A callee's loan is re-minted onto
 * the clones only when **every** member of its storage set is a value this clone made; a set holding
 * a *parameter* takes the whole loan back to the empty one, and `fn tagOf(p: 'Held) -> 'Int = p.tag`
 * is that shape in four tokens - `attachLoan` roots the returned borrow in `%0`, the parameter, and
 * the copy therefore hands the caller a borrow rooted nowhere where the call's own result was rooted
 * in the caller's slot. Every use of it is then outside what `verifyBody`'s third rule can measure,
 * and the `drop` of that slot a line below is an endpoint the rule would otherwise be measured
 * against.
 *
 * That is a **verification** gap and not a live defect, and the difference is which direction the
 * passes below move an instruction. The clone's uses land exactly where the callee wrote them, which
 * is strictly in front of the call the splice replaced; `opt/memory.cpp` moves nothing at all and
 * substitutes only `Trivial` for `Trivial`; `opt/cfg.cpp`'s merge relinks a block's instructions in
 * order in front of its predecessor's terminator; `opt/select.cpp` moves an arm's instructions *up*
 * into the head, which is earlier on the path they were already on and no later on the other; and
 * `simplify`'s CSE removes a use rather than making one. So nothing here can carry such a use past
 * an endpoint - and the day something can, this is the rule that will not report it. What closes it
 * is the union of the borrowing arguments' lenders with the `->` case above excluded, which needs
 * the argument's own use kind at the site rather than the loan alone.
 *
 * **Continuation regions**, re-created under the call site's. A cloned region keeps its kind and its
 * multiplicity; the callee's *root* region maps to the region the call site's block is in, so a
 * cleanup arm inside the callee becomes a cleanup arm nested wherever the call was. `entry` falls
 * out of `Editor::addBlock`, which claims the first block made in a region - and reverse postorder
 * visits a region's entry before any other block of it, since it is the one point control enters at.
 * `destination` is set with `rejoinRegion` from the mapped block.
 *
 * **Allocations stay where they are cloned**, which is inside the caller's own body wherever the
 * callee put them, and that is a decision. `LowerInstAlloca` with a constant size becomes a *frame
 * object* - `collectFrameObjects` in `codegen/x64/frame.cpp` - so an allocation is one slot per
 * instruction however many times control reaches it, and `promoteStackSlots` asks about the stores
 * and loads rather than about where the slot was reserved. A callee inlined inside a loop therefore
 * reuses one slot per iteration, which is correct because each iteration initializes it before
 * reading it and releases it before leaving: that is what the callee's own body already did between
 * its entry and its `return`.
 *
 * ## What is declined, and why each
 *
 *  - **a call that does not name a declaration.** `CallThrough::ClassSlot`, `FunctionSlot` and
 *    `Value` all resolve through something only the lowering can read - a witness slot, a word of a
 *    function value - so there is no body in hand. The one case worth taking is a `Value` call whose
 *    target is a `FunValue` of a non-capturing function in the same block, and it is left for a
 *    later round: what makes it pay is an adaptor chain, and this stage does not build one yet.
 *  - **a callee with no `ssa` body** - a signature, an intrinsic, or a body the conversion declined.
 *  - **an `Erased` generic form.** It is called across the erased ABI with a witness table as a
 *    leading argument, and its body reads its own environment's slots (`gen.value`, a property
 *    projection, a descriptor teardown). Those slots are numbered against *that* declaration's
 *    schema, so a copy of the body inside another function reads the wrong table.
 *  - **a closure or an outlined continuation** - `Function::environment` - and **a lens**
 *    (`Function::lens`, `iterator`, or a `continuationArg`). An outlined continuation takes the
 *    caller's frame by reference and is called with it as its last argument; embedding one is the
 *    *embed* lowering `ssa::Continuation`'s note is about, which needs a region kind that has no
 *    producer yet, and it is a different transformation from this one.
 *  - **recursion**, direct or mutual. Copying a recursive body into a caller copies the recursive
 *    call with it, which is unrolling rather than inlining - a different transformation with a
 *    growth rate nothing here bounds. The call graph's cycles are found once, before the rounds,
 *    which is sound rather than a shortcut: inlining gives a caller edges only to things it could
 *    already reach through the call that went away, so no cycle is made and none is destroyed. The
 *    shipping stage's one exception - `collapsesCycle`, taking one *other* member of the caller's
 *    own cycle - is deliberately not ported: it is what
 *    [[yana-inliner-lens-recursion-runaway]] is open on, where a lens call site inside a
 *    self-recursive function does not converge at all.
 *  - **a callee that may suspend**, read off its published `Summary`. It answers false for every
 *    function today - nothing in this stage is a suspension point - so this is a row rather than a
 *    case, and it is here because the milestone that makes one true is not the milestone that would
 *    think to look here.
 *  - **a `return` in a region that is not the body's own.** A region may be left only for its
 *    declared `destination`, so a `return` inside a cleanup arm would become a jump out of the arm
 *    to somewhere the arm does not rejoin at. Nothing produces one - a cleanup arm is a drop and a
 *    jump back - and it is checked rather than argued because the verifier's rule is the one that
 *    would report it, two passes later.
 *  - **a result the two ends disagree about.** The call's result occurrence is the signature's read
 *    at the call, and the `return` operand's is the same signature's read inside the callee, so they
 *    agree by construction - except where the body's own value is a restatement of the declared
 *    result, which `Conversion::emitLeave` already handles for the `@inline(always)` graft with a
 *    `represent`. The same restatement is made here, and a *tracked* value that disagrees declines
 *    the call rather than being restated, because an owner's declaration and its `return` agree to
 *    the letter and anything else is a defect rather than a coercion.
 *
 * A `Native` is **not** declined. A system call is an ordinary effect, and nothing in a cloned one
 * names the callee's frame: its operands are values, mapped like any other, and the frame it would
 * have addressed is the storage the clone brought with it.
 *
 * ## The cost model
 *
 * Two numbers and one exemption, where the shipping stage has fourteen terms and a per-target table.
 * That is a starting point rather than a claim: its numbers are calibrated against a particular
 * backend's register pressure ([[yana-inline-limits-vs-llvm]] - budget 20 against LLVM's 50, and
 * raising it is +10.7% because the graft enlarges a loop nest the allocator then spills through),
 * and `test/bench/programs` does not compile under this stage yet, so there is nothing here that
 * could rank a second term honestly. What is measurable is the fixture corpus, and the note in
 * `opt/README.md` is what it measured.
 *
 * **The callee is settled first.** `settleCallee` runs `optimizeBody` on it once, lazily, before any
 * site is weighed against it - [[yana-inline-size-honesty]] is the whole of the argument: a body in
 * the form the conversion wrote holds an `alloc`, a store per write and a load per read for every
 * local, and all of that is gone by the time anything emits it. Judging a body three times its own
 * size is measuring the wrong body, not being conservative.
 *
 * **The size excludes what costs nothing to copy**: a `const` is an immediate at its use, an `alloc`
 * is a frame object rather than an instruction, a terminator is the block boundary the copy already
 * paid for in `blocks`, and a `field.addr`/`element.addr` is address arithmetic the lowering folds
 * into the access it feeds. What is left is the work.
 *
 * **The sole call site is the exemption**, and it is a size win rather than a speed one: a callee
 * this is the only reference to is a body that *moves* rather than one that is copied, and `reach`
 * running after the round is what then takes the original away. "Only reference" is stricter than
 * "only call": a function value naming it, a witness table holding it, and `Function::teardown` all
 * mean the declaration outlives the call, and each is counted.
 */

namespace ssa {

namespace {

/*
 * A cap on the cascade rather than a termination proof, on the same terms as the driver's own round
 * limit. A chain of callees each of which calls the next collapses one level per round; recursion is
 * refused outright, so what is left for this to bound is a call depth deeper than three, which stops
 * being inlined rather than going wrong.
 */
constexpr Size kMaxInlineRounds = 3;

/*
 * What a callee may cost, by `settings.inlining`.
 *
 * Three numbers, and the ceiling is the one that is not a budget: it is the size past which the sole
 * call site stops helping, so that a body of any size is not copied on the strength of one reason.
 *
 * `budget` at `Balanced` is twenty, which is the shipping stage's `Speed` number rather than its
 * `Balanced` eight. The two are not comparable and taking the larger is the honest translation:
 * over there the eight is a base that fourteen bonuses are added to and four penalties subtracted
 * from, and here there are none - a callee is weighed on its size and on whether it is the only
 * reference to its body. What the number cannot be here is *measured*: `test/bench/programs` does
 * not compile under this stage, so the corpus that ranks a budget is out of reach and the note in
 * README.md says so rather than implying a measurement.
 */
struct Budget {
    // What a callee may cost to be copied wherever it is called.
    U32 budget = 0;

    // And what it may cost where this is the only reference to it - a *second budget* rather than a
    // bonus added to the first, because the two price different things: one is a copy and the other
    // is a move.
    U32 soleBudget = 0;

    U32 ceiling = 0;
};

Budget budgetFor(InlineLevel level) {
    switch(level) {
        case InlineLevel::None:
            return Budget { 0, 0, 0 };

        // Only the case that cannot grow the program, which is why the budget is zero: a callee
        // qualifies here through the sole call site and through nothing else.
        case InlineLevel::Size:
            return Budget { 0, 96, 96 };

        case InlineLevel::Balanced:
            return Budget { 20, 76, 96 };

        case InlineLevel::Speed:
            return Budget { 40, 120, 160 };
    }

    return Budget();
}

/*
 * How many times a declaration is named, split by whether the reference is a call this pass could
 * remove - `countReferences`.
 *
 * The split is the whole of what the sole-call-site rule rests on. A direct call is a reference that
 * goes away when the call is spliced; a function value, a witness table entry and being a teardown
 * are references the lowering follows and this pass cannot see removed, so a callee with one of them
 * is one whose body stays whatever happens to the call.
 */
struct References {
    U32 calls = 0;
    U32 other = 0;
};

/*
 * The mapping one clone carries - `cloneBodyInto`'s state.
 *
 * Four tables indexed by the callee's own dense ids, which is what makes every lookup an array read
 * rather than a hash: `ValueId`, `BlockId`, `LoanId` and `ContinuationId` are all dense per body.
 */
struct Clone {
    Body& source;

    Array<SsaPtr<Value>> values;

    // Which of those are the callee's **parameters**, which map to the caller's own values rather
    // than to a clone. `mintLoans` is the reader: a borrowed argument's loan was decided where the
    // argument was made and is not this body's to re-mint.
    Array<bool> parameters;

    Array<SsaPtr<Block>> blocks;
    Array<SsaPtr<Loan>> loans;
    Array<ContinuationId> regions;

    // The callee's reachable blocks, in the order they are cloned - see the file comment.
    Array<U32> order;

    // Where a cloned `return` goes, and what it hands over. Null in the straight-line case, which
    // has no edge at all.
    Block* join = nullptr;
    Value* result = nullptr;
};

struct Inliner {
    OptContext& opt;
    Budget limits;

    // Which functions are in a call-graph cycle - `findRecursion`, computed once before the rounds.
    HashMap<U32, bool> recursive;

    // Per callee, this round's reference counts and whether its body has been settled.
    HashMap<U32, References> references;
    HashMap<U32, bool> settled;

    Array<SsaPtr<Inst>> pending;

    SsaBase base() { return opt.base; }
    DeclBase decl() { return opt.decl; }

    /*
     * The callee, folded to a fixed point before any site is weighed against it - and the caller's
     * position given back afterwards.
     *
     * Lazily and once. This pass walks every function of every module, Core and the prelude
     * included, and a program reaches a few dozen of them; settling only the ones something calls is
     * what keeps this from being the driver's own loop run twice. What it costs on top is nothing,
     * because the driver optimizes each of those bodies afterwards anyway and now starts from a
     * fixed point.
     *
     * `opt.function` and `opt.body` are the caller's and are restored, which is not a detail: the
     * analysis caches are stamped with the function they were read from, so a settle in the middle
     * of a walk that left the wrong one current would hand the caller a dominator tree of the
     * callee's.
     */
    void settle(mir::Function& function) {
        if(!function.ssa) return;

        auto caller = opt.function;
        auto body = opt.body;

        opt.function = &function;
        opt.body = base()[function.ssa];
        optimizeBody(opt);

        opt.function = caller;
        opt.body = body;
    }

    void settleCallee(DeclPtr<mir::Function> pointer) {
        auto entry = settled.add(pointer.offset);
        if(entry.existed && *entry.value) return;

        *entry.value = true;
        settle(*decl()[pointer]);
    }

    /*
     * What a body costs to copy, in instructions - see the file comment for what is excluded and
     * why each of them is.
     *
     * Over the reachable blocks only, which is the set the clone actually copies.
     */
    U32 sizeOf(mir::Function& function) {
        auto body = base()[function.ssa];

        Array<U32> order;
        reversePostorder(base(), *body, order);

        U32 size = 0;

        for(auto index: order) {
            auto block = body->block(base(), BlockId(index));

            for(auto pointer: block->instructions(base())) {
                switch(base()[pointer]->kind) {
                    case Inst::Const:
                    case Inst::Alloc:
                    case Inst::FieldAddress:
                    case Inst::ElementAddress:
                    case Inst::Return:
                    case Inst::Jump:
                    case Inst::Branch:
                    case Inst::Unreachable:
                        break;

                    default:
                        size++;
                        break;
                }
            }
        }

        return size;
    }

    /*
     * Whether the callee's shape is one this pass reproduces - the file comment's list, asked once
     * per site because every clause is about the declaration rather than about the site.
     */
    bool clonableDeclaration(mir::Function& caller, DeclPtr<mir::Function> pointer) {
        auto callee = decl()[pointer];

        if(callee == &caller) return false;
        if(!callee->ssa || !callee->reachable) return false;

        // A generic compiled once, and a closure or a continuation. Both read something the copy
        // cannot bring with it: a slot of the callee's own environment, or the caller's frame.
        if(callee->form == mir::GenForm::Erased && mir::isGeneric(decl(), callee->gen)) return false;
        if(callee->environment) return false;

        if(callee->lens || callee->iterator) return false;
        if(callee->continuationArg != mir::Function::kNoContinuation) return false;

        // A suspension has no representation at this tier, so this is a row rather than a case - see
        // the file comment.
        if(callee->summary.suspension().get(true)) return false;

        return !recursive.getValue(pointer.offset);
    }

    /*
     * And the two clauses that are about the body rather than about the declaration - asked after
     * `settleCallee`, since what is being weighed and copied is the settled form.
     */
    bool clonableBody(mir::Function& callee) {
        auto body = base()[callee.ssa];
        auto region = base()[body->entry]->region;

        for(U32 i = 0; i < body->blocks.size(); i++) {
            auto block = body->block(base(), BlockId(i));
            if(block->deleted) continue;

            for(auto instPointer: block->instructions(base())) {
                auto inst = base()[instPointer];

                // A `return` that is not in the body's own region - the file comment's clause, and
                // the one thing about a *callee* that the region rule can refuse.
                if(inst->kind == Inst::Return && block->region != region) return false;

                // A read of the callee's own generic environment. Unreachable given the erased
                // refusal above, and checked because the two are different statements.
                if(inst->kind == Inst::GenValue) return false;

                if(inst->kind == Inst::FieldAddress &&
                   ((InstFieldAddress&)*inst).property != InstFieldAddress::kNoProperty) {
                    return false;
                }
            }
        }

        return true;
    }

    /*
     * Whether the copy is worth making - the two numbers and the exemption.
     *
     * `@inline(always)` reaching here at all means the conversion's graft declined it, which happens
     * at eight expansions deep and nowhere else; it is weighed against the ceiling alone, because
     * the attribute is a directive the declaration was already checked for.
     */
    bool worthInlining(DeclPtr<mir::Function> pointer) {
        if(!limits.ceiling) return false;

        auto callee = decl()[pointer];
        auto size = sizeOf(*callee);
        if(size > limits.ceiling) return false;

        if(callee->alwaysInline) return true;
        if(size <= limits.budget) return true;

        auto found = references.getValue(pointer.offset);
        if(!found) return false;

        auto counts = found.unwrap();
        if(counts.other || counts.calls != 1) return false;
        if(callee->teardown || pointer == opt.program.entry) return false;

        return size <= limits.soleBudget;
    }

    // ---------------------------------------------------------------- the clone

    Value* mapValue(Clone& clone, SsaPtr<Value> value) {
        auto index = base()[value]->id.index;
        assertTrue(index < clone.values.size() && clone.values[index]);
        return base()[clone.values[index]];
    }

    Use mapUse(Clone& clone, Use used) {
        if(!used.isSet()) return Use();
        return Use(mapValue(clone, used.value()) - base(), used.kind());
    }

    Block* mapBlock(Clone& clone, SsaPtr<Block> block) {
        auto index = base()[block]->index.index;
        assertTrue(index < clone.blocks.size() && clone.blocks[index]);
        return base()[clone.blocks[index]];
    }

    void mapUses(Clone& clone, UseList& from, SmallArray<Use, 8>& into) {
        for(auto used: from.contents(base())) into.push(mapUse(clone, used));
    }

    /*
     * One instruction re-created through the editor, at whatever insertion point the caller set.
     *
     * Through the editor rather than by copying the record, which is what keeps the use lists, the
     * results and the loan defaults right by construction: every `add*` records its operands and
     * gives its result the occurrence and convention it is handed. The result's occurrence and
     * convention are read off the *original's* result rather than re-derived, so nothing here has to
     * ask `conventionFor` a second time.
     */
    void cloneInst(Clone& clone, Editor& editor, Inst& from) {
        auto at = from.source;
        auto results = from.results.size();
        auto original = results ? base()[from.results.get(base(), 0)] : nullptr;

        auto occurrence = [&] { return original->occurrence; };
        auto convention = [&] { return original->convention; };

        Inst* made = nullptr;

        switch(from.kind) {
            case Inst::Const:
                made = editor.addConst(at, ((InstConst&)from).constant);
                break;

            case Inst::GlobalAddress:
                made = editor.addGlobalAddress(at, ((InstGlobalAddress&)from).global);
                break;

            case Inst::Metric: {
                auto& written = (InstMetric&)from;
                made = editor.addMetric(at, written.metric, written.of, occurrence().type);
                break;
            }

            case Inst::Native: {
                auto& written = (InstNative&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.arguments, arguments);

                made = editor.addNative(at, written.op, arguments.pointer(), arguments.size(),
                                        results ? occurrence() : Occurrence(),
                                        results ? convention() : Convention::Trivial, results != 0);
                break;
            }

            case Inst::Atomic: {
                auto& written = (InstAtomic&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.arguments, arguments);

                made = editor.addAtomic(at, written.op, written.order, written.failure, written.weak,
                                        arguments.pointer(), arguments.size(),
                                        results ? occurrence() : Occurrence(),
                                        results ? convention() : Convention::Trivial, results != 0);
                break;
            }

            case Inst::AtomicOk:
                made = editor.addAtomicOk(at, mapUse(clone, ((InstAtomicOk&)from).cas), occurrence());
                break;

            case Inst::Fma: {
                auto& written = (InstFma&)from;
                made = editor.addFma(at, mapUse(clone, written.lhs), mapUse(clone, written.rhs),
                                     mapUse(clone, written.addend), occurrence());
                break;
            }

            case Inst::Select: {
                auto& written = (InstSelect&)from;
                made = editor.addSelect(at, mapUse(clone, written.condition),
                                        mapUse(clone, written.whenTrue),
                                        mapUse(clone, written.whenFalse), occurrence(), convention());
                break;
            }

            case Inst::VecSplat:
                made = editor.addVecSplat(at, mapUse(clone, ((InstVecSplat&)from).from), occurrence());
                break;

            case Inst::VecLane: {
                auto& written = (InstVecLane&)from;
                made = editor.addVecLane(at, mapUse(clone, written.from), written.lane, occurrence());
                break;
            }

            case Inst::VecWithLane: {
                auto& written = (InstVecLane&)from;
                made = editor.addVecWithLane(at, mapUse(clone, written.from), written.lane,
                                             mapUse(clone, written.value), occurrence());
                break;
            }

            case Inst::VecShuffle: {
                auto& written = (InstVecShuffle&)from;
                SmallArray<U8, 16> pattern;
                for(auto lane: written.pattern.contents(base())) pattern.push(lane);

                made = editor.addVecShuffle(at, mapUse(clone, written.left),
                                            mapUse(clone, written.right), pattern.pointer(),
                                            pattern.size(), occurrence());
                break;
            }

            case Inst::VecReduce: {
                auto& written = (InstVecReduce&)from;
                made = editor.addVecReduce(at, mapUse(clone, written.from), written.reduce,
                                           occurrence());
                break;
            }

            case Inst::Alloc: {
                auto& written = (InstAlloc&)from;
                made = editor.addAlloc(at, written.held, written.storage);
                break;
            }

            case Inst::FieldAddress: {
                auto& written = (InstFieldAddress&)from;
                made = editor.addFieldAddress(at, mapUse(clone, written.base), written.record,
                                              written.field, written.shape);
                break;
            }

            case Inst::ElementAddress: {
                auto& written = (InstElementAddress&)from;
                made = editor.addElementAddress(at, mapUse(clone, written.base),
                                                mapUse(clone, written.index), written.container);
                break;
            }

            case Inst::Load:
                made = editor.addLoad(at, mapUse(clone, ((InstLoad&)from).address), occurrence(),
                                      convention());
                break;

            case Inst::Copy:
                made = editor.addCopy(at, mapUse(clone, ((InstCopy&)from).address), occurrence(),
                                      convention());
                break;

            case Inst::Store: {
                auto& written = (InstStore&)from;
                made = editor.addStore(at, mapUse(clone, written.address),
                                       mapUse(clone, written.value));
                break;
            }

            case Inst::Binary: {
                auto& written = (InstBinary&)from;
                made = editor.addBinary(at, written.op, mapUse(clone, written.lhs),
                                        mapUse(clone, written.rhs), occurrence(), convention());
                break;
            }

            case Inst::Unary: {
                auto& written = (InstUnary&)from;
                made = editor.addUnary(at, written.op, mapUse(clone, written.value), occurrence(),
                                       convention());
                break;
            }

            case Inst::Aggregate: {
                auto& written = (InstAggregate&)from;
                SmallArray<Use, 8> components;
                mapUses(clone, written.components, components);

                made = editor.addAggregate(at, written.constructor, components.pointer(),
                                           components.size(), occurrence(), convention());
                break;
            }

            case Inst::Drop: {
                auto& written = (InstDrop&)from;
                made = editor.addDrop(at, mapUse(clone, written.address), written.held,
                                      written.boxesVisible);
                break;
            }

            case Inst::FunValue: {
                auto& written = (InstFunValue&)from;
                auto environment = mapUse(clone, written.environment);

                made = editor.addFunValue(at, written.callee,
                                          written.capturing ? &environment : nullptr, occurrence(),
                                          convention());
                break;
            }

            case Inst::Call: {
                auto& written = (InstCall&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.arguments, arguments);

                /*
                 * The call site rebuilt out of the fields it was built from. `environment` travels
                 * unchanged and needs no mapping: a `mir::GenEnvironment` is a declaration in the
                 * shared region, interned per (generic, argument list), so the witness table this
                 * call hands over is the same table wherever the call stands.
                 */
                mir::CallSite site;
                site.target = written.callee;
                site.through = written.through;
                site.requirement = written.requirement;
                site.member = written.member;
                site.environment = written.environment;

                made = editor.addCall(at, site, mapUse(clone, written.target), arguments.pointer(),
                                      arguments.size(), results ? occurrence() : Occurrence(),
                                      results ? convention() : Convention::Trivial, results != 0);
                break;
            }

            case Inst::GenValue:
            case Inst::Return:
            case Inst::Jump:
            case Inst::Branch:
            case Inst::Unreachable:
            case Inst::KindCount:
                // The terminators are `cloneTerminator`'s, and the two refused kinds never reach a
                // body this pass admits - see `clonableBody`.
                assertTrue(false);
                return;
        }

        assertTrue(made != nullptr && made->results.size() == results);
        for(Size i = 0; i < results; i++) {
            clone.values[base()[from.results.get(base(), i)]->id.index] =
                editor.resultOf(*made, i) - base();
        }
    }

    /*
     * A returned value restated as the occurrence the call's result declares - the same handover
     * `Conversion::emitLeave` makes for the `@inline(always)` graft, and for the same reason.
     *
     * A `values`-shaped body returns the pointer it read out of a field, an owned `%a`, at a result
     * declared `%source'a`. A call boundary tolerates that silently because the callee's `return` is
     * not held to the declaration and the caller reads the declaration; a jump is held to its
     * target's argument and a substitution is held to its readers, so the restatement has to be
     * explicit. `represent` moves no bytes and the lowering passes the value through.
     */
    Value* restated(Editor& editor, Value& supplied, Value& declared, LocationId at) {
        if(supplied.occurrence == declared.occurrence) return &supplied;

        auto inst = editor.addUnary(at, mir::UnaryOp::Represent,
                                    editor.use(supplied, UseKind::Inspect), declared.occurrence,
                                    declared.convention);

        return editor.resultOf(*inst);
    }

    // Whether the two ends of the handover agree well enough to be joined - see the file comment's
    // last decline.
    bool handoverAgrees(Value* returned, Value* result) {
        if(!returned || !result) return returned == nullptr && result == nullptr;
        if(returned->convention != result->convention) return false;

        // A restatement is a `represent`, which is a pure instruction and may not consume - so a
        // tracked value that disagrees is a defect rather than something to coerce.
        return returned->occurrence == result->occurrence || !isTracked(returned->convention);
    }

    void cloneTerminator(Clone& clone, Editor& editor, Block& into, Inst& from) {
        auto at = from.source;

        switch(from.kind) {
            case Inst::Return: {
                auto& written = (InstReturn&)from;
                Value* returned = written.values.size()
                    ? mapValue(clone, written.values.get(base(), 0).value()) : nullptr;

                if(!clone.join) {
                    // The straight-line case has no edge: the value is handed to the splice, which
                    // points the call's readers at it.
                    clone.result = returned;
                    return;
                }

                if(!returned) {
                    editor.replaceTerminator(into, editor.makeJump(at, *clone.join, nullptr, 0));
                    return;
                }

                auto declared = base()[clone.join->argumentAt(base(), 0)];
                editor.setBlock(into);

                auto value = restated(editor, *returned, *declared, at);
                auto used = editor.use(*value, written.values.get(base(), 0).kind());
                editor.replaceTerminator(into, editor.makeJump(at, *clone.join, &used, 1));
                return;
            }

            case Inst::Jump: {
                auto& written = (InstJump&)from;
                SmallArray<Use, 8> arguments;
                mapUses(clone, written.to.arguments, arguments);

                editor.replaceTerminator(into,
                    editor.makeJump(at, *mapBlock(clone, written.to.target), arguments.pointer(),
                                    arguments.size()));
                return;
            }

            case Inst::Branch: {
                // Each edge's own operands, which is `makeBranchEdges`' reason to exist: `promote`
                // places a block argument where one slot's writes meet, so the two arms of a branch
                // in a settled callee routinely declare different ones.
                auto& written = (InstBranch&)from;
                SmallArray<Use, 8> whenTrue;
                SmallArray<Use, 8> whenFalse;
                mapUses(clone, written.whenTrue.arguments, whenTrue);
                mapUses(clone, written.whenFalse.arguments, whenFalse);

                editor.replaceTerminator(into,
                    editor.makeBranchEdges(at, mapUse(clone, written.condition),
                                           *mapBlock(clone, written.whenTrue.target),
                                           whenTrue.pointer(), whenTrue.size(),
                                           *mapBlock(clone, written.whenFalse.target),
                                           whenFalse.pointer(), whenFalse.size()));
                return;
            }

            default:
                editor.replaceTerminator(into, editor.makeUnreachable(at));
                return;
        }
    }

    /*
     * The loans, re-minted in the caller's table once every value exists - the file comment's third
     * mapping.
     *
     * After the instructions rather than before them, because a loan names *storage values* and a
     * value names its loan: minting first would need the clones of the allocations, and setting the
     * dependency first would need the loans. The two halves are separate operations on the editor
     * for exactly this reason - `Editor::addResult` gives every borrow the caller's loan and
     * `setDependency` refines it - so the order falls out.
     */
    void mintLoans(Clone& clone, Editor& editor) {
        clone.loans.reserve(clone.source.loans.size());

        for(U32 i = 0; i < clone.source.loans.size(); i++) {
            auto loan = clone.source.loan(base(), LoanId(i));

            SmallArray<SsaPtr<Value>, 4> storage;
            auto complete = true;

            for(auto held: loan->storage.contents(base())) {
                auto index = base()[held]->id.index;

                /*
                 * A lender that is a **parameter**, or one in a block the clone did not copy.
                 *
                 * Both take the loan back to the caller's empty set, which never ends inside this
                 * body - the file comment's third mapping and its argument. A parameter's lender is
                 * the caller's storage and the caller's own loan already names it; re-minting one
                 * here would root every unrefined borrow of the callee in one argument's storage.
                 */
                if(index >= clone.values.size() || !clone.values[index] || clone.parameters[index]) {
                    complete = false;
                    break;
                }

                storage.push(clone.values[index]);
            }

            if(!complete) storage.clear();

            clone.loans.push(editor.loanOn(loan->source, storage.pointer(), storage.size(),
                                           loan->capability) - base());
        }

        for(U32 i = 0; i < clone.source.values.size(); i++) {
            if(i >= clone.values.size() || !clone.values[i] || clone.parameters[i]) continue;

            auto value = clone.source.value(base(), ValueId(i));
            if(value->convention != Convention::Guaranteed || !value->loan) continue;

            auto source = base()[value->loan]->id.index;
            if(source >= clone.loans.size()) continue;

            editor.setDependency(*base()[clone.values[i]], *base()[clone.loans[source]]);
        }
    }

    // ---------------------------------------------------------------- the splice

    /*
     * The parameters bound to the call's operands, which is the whole of what a callee's entry block
     * becomes - `resolve_ssa/README.md`'s "parameters arrive as entry-block arguments" read from the
     * caller's side.
     *
     * `parameters` is the other half and is what `mintLoans` reads: these positions map to values the
     * *caller* owns, so nothing about their loans is this clone's to decide.
     */
    void bindParameters(Clone& clone, Block& entry, InstCall& call) {
        auto count = clone.source.values.size();

        clone.values.reserve(count);
        clone.parameters.reserve(count);

        for(U32 i = 0; i < count; i++) {
            clone.values.push(nullptr);
            clone.parameters.push(false);
        }

        clone.blocks.reserve(clone.source.blocks.size());
        for(U32 i = 0; i < clone.source.blocks.size(); i++) clone.blocks.push(nullptr);

        for(Size i = 0; i < entry.argumentCount(); i++) {
            auto index = base()[entry.argumentAt(base(), i)]->id.index;
            clone.values[index] = call.arguments.get(base(), i).value();
            clone.parameters[index] = true;
        }
    }

    /*
     * A callee of one block ending in `return`, spliced into the caller's own block.
     *
     * The whole of it is a value map: the instructions are re-created in front of the call, the
     * `return` hands back a value, and every reader of the call's result is pointed at it. No block
     * is made and no edge moves, which is what makes this the shape that keeps the dump readable and
     * the block-local passes below still able to see the callee's storage.
     */
    void spliceStraightLine(Editor& editor, mir::Function& callee, InstCall& call, Block& block) {
        auto body = base()[callee.ssa];
        auto entry = base()[body->entry];

        Clone clone { *body };
        bindParameters(clone, *entry, call);
        clone.blocks[entry->index.index] = &block - base();

        editor.setInsertBefore(call);

        for(auto pointer: entry->instructions(base())) {
            auto inst = base()[pointer];
            if(isTerminator(inst->kind)) break;

            cloneInst(clone, editor, *inst);
        }

        // Which sets `clone.result` rather than editing anything: with no join there is no edge for
        // a `return` to become.
        cloneTerminator(clone, editor, block, *entry->terminator(base()));
        mintLoans(clone, editor);

        if(call.results.size()) {
            auto result = base()[call.results.get(base(), 0)];
            editor.setInsertBefore(call);
            editor.replaceAllUses(*result, *restated(editor, *clone.result, *result, call.source));
        }

        editor.eraseInst(call);
    }

    /*
     * The general form: the caller's block cut in two, the callee's blocks in the gap, and a join
     * carrying the result.
     *
     * The join is a block of its own rather than the second half of the split - see the file
     * comment. What makes that cheap is that `cleanCfg` runs immediately below this in the driver
     * and folds the pair, since the join's one successor has exactly one predecessor.
     */
    void spliceControlFlow(Editor& editor, mir::Function& callee, InstCall& call, Block& block,
                           Size position) {
        auto body = base()[callee.ssa];
        auto entry = base()[body->entry];
        auto result = call.results.size() ? base()[call.results.get(base(), 0)] : nullptr;

        Clone clone { *body };
        reversePostorder(base(), *body, clone.order);
        bindParameters(clone, *entry, call);

        auto continuation = editor.splitBlock(block, position + 1);

        // Every block this makes is in the call site's region, which the attaching editor does not
        // start in: its cursor is the entry block's.
        editor.setRegion(block.region);

        auto join = editor.addBlock(block.name);
        clone.join = join;

        if(result) editor.addBlockArgument(*join, result->occurrence, result->convention);
        editor.replaceTerminator(*join, editor.makeJump(call.source, *continuation, nullptr, 0));

        /*
         * The regions, before any block is made: a block names the region it is in when it is
         * created, and `addBlock` claims the first one made in a region as its `entry`.
         *
         * The callee's root maps to the region the call site is in. Every other one is re-created
         * under its own mapped parent, which is why the walk is in id order - `enterRegion` takes
         * the parent from the cursor and a region is opened inside one that already exists, so a
         * parent's id is always below its child's.
         */
        clone.regions.reserve(body->regions.size());
        for(U32 i = 0; i < body->regions.size(); i++) clone.regions.push(ContinuationId());

        for(U32 i = 0; i < body->regions.size(); i++) {
            auto region = body->region(base(), ContinuationId(i));

            if(!region->parent.valid()) {
                clone.regions[i] = block.region;
                continue;
            }

            editor.setRegion(clone.regions[region->parent.index]);
            clone.regions[i] = editor.enterRegion(region->kind, region->multiplicity);
            editor.leaveRegion();
        }

        for(auto index: clone.order) {
            auto from = body->block(base(), BlockId(index));
            editor.setRegion(clone.regions[from->region.index]);
            clone.blocks[index] = editor.addBlock(from->name) - base();
        }

        editor.setRegion(block.region);

        // The destinations, once every block exists - `rejoinRegion` names a block outside the
        // region it belongs to, so both have to be mapped first.
        for(U32 i = 0; i < body->regions.size(); i++) {
            auto region = body->region(base(), ContinuationId(i));
            if(!region->parent.valid() || !region->destination.valid()) continue;

            auto destination = clone.blocks[region->destination.index];
            if(destination) editor.rejoinRegion(clone.regions[i], *base()[destination]);
        }

        // Every block's arguments before any instruction anywhere, which is `addBlockArgument`'s own
        // rule: a parameter holds on entry, so a block that has been filled refuses one.
        for(auto index: clone.order) {
            if(BlockId(index) == entry->index) continue;

            auto from = body->block(base(), BlockId(index));
            auto target = base()[clone.blocks[index]];

            for(auto pointer: from->arguments(base())) {
                auto argument = base()[pointer];
                clone.values[argument->id.index] =
                    editor.addBlockArgument(*target, argument->occurrence,
                                            argument->convention) - base();
            }
        }

        for(auto index: clone.order) {
            auto from = body->block(base(), BlockId(index));
            editor.setBlock(*base()[clone.blocks[index]]);

            for(auto pointer: from->instructions(base())) {
                auto inst = base()[pointer];
                if(isTerminator(inst->kind)) break;

                cloneInst(clone, editor, *inst);
            }
        }

        // The terminators last, because one names blocks and block arguments the loop above is what
        // makes exist.
        for(auto index: clone.order) {
            auto from = body->block(base(), BlockId(index));
            cloneTerminator(clone, editor, *base()[clone.blocks[index]], *from->terminator(base()));
        }

        mintLoans(clone, editor);

        // And the way in, which is the last edit that touches the caller's own block: the call is
        // still standing in it and is erased once nothing reads its result.
        editor.replaceTerminator(block, editor.makeJump(call.source,
                                                        *base()[clone.blocks[entry->index.index]],
                                                        nullptr, 0));

        if(result) editor.replaceAllUses(*result, *base()[join->argumentAt(base(), 0)]);
        editor.eraseInst(call);
    }

    Size reachableBlockCount(Body& body) {
        Array<U32> order;
        reversePostorder(base(), body, order);
        return order.size();
    }

    /*
     * Whether every `return` in the callee hands over what the call's result declares - asked before
     * anything is edited, because a splice half made cannot be taken back.
     *
     * And whether there is one at all, which is the other half of the same question. A callee that
     * cannot return - one settled down to an endless loop, whose exit test the folder decided and
     * `cleanCfg` then deleted the arm of - reaches the general splice with **no** cloned `return` to
     * jump to the join. The join is then a block nothing enters, holding the block argument every
     * reader of the call's result has just been pointed at, and the continuation reads a value no
     * path defines: `verifyBody` reports it at this pass's own checkpoint. What a splice would buy
     * here is the caller's tail becoming unreachable, which is the correct answer and is a different
     * rewrite - deleting the continuation, not filling it - so the call is declined instead.
     */
    bool handoverChecks(Body& body, Value* result) {
        Array<U32> order;
        reversePostorder(base(), body, order);

        auto returns = false;

        for(auto index: order) {
            auto terminator = body.block(base(), BlockId(index))->terminator(base());
            if(!terminator || terminator->kind != Inst::Return) continue;

            returns = true;

            auto& written = (InstReturn&)*terminator;
            if(written.values.size() != Size(result ? 1 : 0)) return false;
            if(!result) continue;

            if(!handoverAgrees(base()[written.values.get(base(), 0).value()], result)) return false;
        }

        return returns;
    }

    bool inlineSite(mir::Function& caller, InstCall& call) {
        if(call.through != mir::CallThrough::Declaration || !call.callee) return false;
        if(!clonableDeclaration(caller, call.callee)) return false;

        /*
         * The callee settled *before* anything about its body is read, which is not only the size
         * question: the rounds rewrite the body this is about to copy, so a refusal or a handover
         * checked against the unsettled form would be a statement about IR that no longer exists.
         */
        settleCallee(call.callee);

        auto callee = decl()[call.callee];
        if(!clonableBody(*callee)) return false;

        auto body = base()[callee->ssa];
        auto entry = base()[body->entry];

        // A signature the site does not fill is a defect rather than a case: the call was built from
        // this declaration's argument list and the entry's arguments are that list.
        assertTrue(entry->argumentCount() == call.arguments.size());
        if(entry->argumentCount() != call.arguments.size()) return false;

        auto result = call.results.size() ? base()[call.results.get(base(), 0)] : nullptr;
        if(!handoverChecks(*body, result)) return false;
        if(!worthInlining(call.callee)) return false;

        auto block = base()[call.block];
        Size position = 0;
        auto found = false;

        for(Size i = 0; i < block->instructionCount(); i++) {
            if(base()[block->instructionAt(base(), i)] != &call) continue;

            position = i;
            found = true;
            break;
        }

        if(!found) return false;

        auto editor = opt.ir();

        if(entry->terminator(base())->kind == Inst::Return && reachableBlockCount(*body) == 1) {
            spliceStraightLine(editor, *callee, call, *block);
        } else {
            spliceControlFlow(editor, *callee, call, *block, position);
        }

        return true;
    }

    // ---------------------------------------------------------------- the driver

    /*
     * How many times each declaration is named, over the bodies the program can still reach.
     *
     * Reachable only, and that is the decided half rather than an economy:
     * [[yana-escape-before-inlining]] item 5 is a call site in a function nothing could run being
     * counted, which turned a callee's one site into two and refused it. `reach` runs immediately in
     * front of this for exactly that reason.
     */
    void countReferences() {
        references.clear();

        auto count = [&](DeclPtr<mir::Function> callee, bool isCall) {
            if(!callee) return;

            auto entry = references.add(callee.offset);
            if(!entry.existed) *entry.value = References();

            if(isCall) entry.value->calls++; else entry.value->other++;
        };

        for(auto modulePointer: opt.program.modules) {
            auto module = decl()[modulePointer];

            for(auto pointer: module->functions.contents(decl())) {
                auto function = decl()[pointer];
                if(!function->reachable || !function->ssa) continue;

                auto body = base()[function->ssa];

                for(U32 i = 0; i < body->blocks.size(); i++) {
                    auto block = body->block(base(), BlockId(i));
                    if(block->deleted) continue;

                    for(auto instPointer: block->instructions(base())) {
                        auto inst = base()[instPointer];

                        if(inst->kind == Inst::Call) {
                            auto& call = (InstCall&)*inst;
                            count(call.callee, call.through == mir::CallThrough::Declaration);

                            if(call.environment) {
                                for(auto witness: decl()[call.environment]->slots.contents(decl())) {
                                    for(auto member: witness.functions.contents(decl())) {
                                        count(member, false);
                                    }
                                }
                            }

                            continue;
                        }

                        if(inst->kind == Inst::FunValue) count(((InstFunValue&)*inst).callee, false);
                    }
                }
            }
        }
    }

    bool runFunction(DeclPtr<mir::Function> pointer) {
        auto& function = *decl()[pointer];

        opt.function = &function;
        opt.body = base()[function.ssa];

        /*
         * Every call in the body, read out before any of them is spliced.
         *
         * A splice moves instructions between blocks and erases the call, so a walk holding a block
         * cursor is a walk over a list that has changed underneath it. What this list can hold that
         * is no longer there is exactly one thing - a call that was erased - and `Inst::block` says
         * so, because `eraseInst` unlinks it. A call the clone *brought* is not in the list and is
         * the next round's.
         */
        pending.clear();

        for(U32 i = 0; i < opt.body->blocks.size(); i++) {
            auto block = opt.body->block(base(), BlockId(i));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(base())) {
                if(base()[pointer]->kind == Inst::Call) pending.push(pointer);
            }
        }

        auto inlined = false;

        for(auto pointer: pending) {
            auto inst = base()[pointer];
            if(!inst->block) continue;

            inlined = inlineSite(function, (InstCall&)*inst) || inlined;
        }

        if(!inlined) return false;

        checkpoint(opt, "after inlineCalls"_v);

        /*
         * And the caller folded back to a fixed point, which is the other half of
         * [[yana-inline-size-honesty]]: the next site weighed against *this* body has to see what a
         * backend would emit rather than what a splice left behind. `settled` is cleared for it so
         * that a later caller re-settles rather than reading the memo taken before the splice.
         */
        settled.remove(pointer.offset);
        settle(function);
        return true;
    }
};

/*
 * Which functions can reach themselves through a chain of calls, found once for the whole program -
 * Tarjan's algorithm, because a strongly connected component *is* a cycle and the one-member case a
 * self-edge makes is the one a component numbering cannot tell from an ordinary function.
 *
 * Computed before the rounds and not between them, which is sound rather than a shortcut: a clone
 * gives its caller edges only to what it could already reach through the call that went away, so no
 * cycle is created and none is destroyed.
 *
 * Every kind of call is an edge here, not only the ones this pass would clone. What the graph is for
 * is deciding whether a *body* is recursive, and a body that reaches itself through a class dispatch
 * is as recursive as one that does not.
 */
void findRecursion(OptContext& opt, HashMap<U32, bool>& recursive) {
    Array<DeclPtr<mir::Function>> nodes;
    HashMap<U32, U32> index;

    Size functionCount = 0;
    for(auto modulePointer: opt.program.modules) {
        functionCount += opt.decl[modulePointer]->functions.size();
    }

    index.reserve(functionCount);
    recursive.reserve(functionCount);
    nodes.reserve(U32(functionCount));

    for(auto modulePointer: opt.program.modules) {
        for(auto pointer: opt.decl[modulePointer]->functions.contents(opt.decl)) {
            auto entry = index.add(pointer.offset);
            if(entry.existed) continue;

            *entry.value = U32(nodes.size());
            nodes.push(pointer);
        }
    }

    // The call graph as one array of targets with a start offset per node, which is two allocations
    // for the graph rather than one per function in the program.
    Array<U32> edgeData;
    Array<U32> edgeStart;

    for(Size i = 0; i < nodes.size(); i++) {
        edgeStart.push(U32(edgeData.size()));

        auto function = opt.decl[nodes[i]];
        if(!function->ssa) continue;

        auto body = opt.base[function->ssa];

        for(U32 b = 0; b < body->blocks.size(); b++) {
            auto block = body->block(opt.base, BlockId(b));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(opt.base)) {
                auto inst = opt.base[pointer];

                DeclPtr<mir::Function> callee = nullptr;
                if(inst->kind == Inst::Call) callee = ((InstCall&)*inst).callee;
                else if(inst->kind == Inst::FunValue) callee = ((InstFunValue&)*inst).callee;

                if(!callee) continue;

                auto found = index.getValue(callee.offset);
                if(found) edgeData.push(found.unwrap());
            }
        }
    }

    edgeStart.push(U32(edgeData.size()));

    auto edgeCount = [&](Size node) { return Size(edgeStart[node + 1] - edgeStart[node]); };
    auto edgeAt = [&](Size node, Size which) { return edgeData[edgeStart[node] + which]; };

    auto callsItself = [&](Size node) {
        for(Size e = 0; e < edgeCount(node); e++) {
            if(edgeAt(node, e) == U32(node)) return true;
        }

        return false;
    };

    auto count = nodes.size();
    Array<U32> number, lowlink;
    ScratchSet onStack(opt.sets, count);

    for(Size i = 0; i < count; i++) {
        number.push(0);
        lowlink.push(0);
    }

    // Explicit stacks rather than recursion: a call chain in a large program is deeper than this
    // process's own stack is willing to be.
    Array<U32> component;
    Array<U32> pending;
    Array<Size> next;
    Array<U32> members;
    U32 counter = 1;

    for(Size root = 0; root < count; root++) {
        if(number[root]) continue;

        number[root] = lowlink[root] = counter++;
        onStack->set(root, true);
        component.push(U32(root));
        pending.push(U32(root));
        next.push(0);

        while(pending.size()) {
            auto node = pending[pending.size() - 1];

            if(next[next.size() - 1] < edgeCount(node)) {
                auto target = edgeAt(node, next[next.size() - 1]++);

                if(!number[target]) {
                    number[target] = lowlink[target] = counter++;
                    onStack->set(target, true);
                    component.push(target);
                    pending.push(target);
                    next.push(0);
                } else if((*onStack)[target] && number[target] < lowlink[node]) {
                    lowlink[node] = number[target];
                }

                continue;
            }

            pending.pop();
            next.pop();

            if(lowlink[node] == number[node]) {
                members.clear();
                while(component.size()) {
                    auto member = component.pop().unwrap();
                    onStack->set(member, false);
                    members.push(member);

                    if(member == node) break;
                }

                // A one-member component is a cycle only through an edge to itself, which is the
                // case the component numbering cannot report.
                if(members.size() == 1 && !callsItself(node)) continue;

                for(auto member: members) recursive.add(nodes[member].offset, true);
            }

            if(pending.size() && lowlink[node] < lowlink[pending[pending.size() - 1]]) {
                lowlink[pending[pending.size() - 1]] = lowlink[node];
            }
        }
    }
}

} // namespace

void inlineCalls(OptContext& opt) {
    Inliner inliner { opt, budgetFor(opt.context.settings.inlining) };

    // Zero only at `InlineLevel::None`, where nothing qualifies and everything below would be a
    // whole-program walk that decided nothing.
    if(!inliner.limits.ceiling) return;

    findRecursion(opt, inliner.recursive);

    auto caller = opt.function;
    auto body = opt.body;

    for(Size round = 0; round < kMaxInlineRounds; round++) {
        /*
         * What the program can still reach, asked again before anything is counted against it - see
         * `reach.cpp`, and [[yana-escape-before-inlining]] item 5 for why a stale answer feeding a
         * threshold is a decided bug rather than a hypothetical one.
         */
        markProgramReachable(opt);
        inliner.countReferences();

        auto inlined = false;

        for(auto modulePointer: opt.program.modules) {
            auto module = opt.decl[modulePointer];

            for(auto pointer: module->functions.contents(opt.decl)) {
                auto function = opt.decl[pointer];

                // A body nothing can reach is not one to copy anything into: it is not emitted, and
                // its own call sites have just stopped being counted.
                if(!function->reachable || !function->ssa) continue;

                inlined = inliner.runFunction(pointer) || inlined;
            }
        }

        if(!inlined) break;
    }

    // And once at the end, so that the per-function loop below this walks what the program actually
    // holds rather than what it held before the last round.
    markProgramReachable(opt);

    opt.function = caller;
    opt.body = body;
}

} // namespace ssa
