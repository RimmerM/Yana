#include "opt_pass.h"

/*
 * The driver, and the questions about types and storage every pass asks.
 *
 * Which passes run and how many times is here; what each of them decides lives in the file that
 * decides it. The loop is a fixed point rather than a sequence because the three feed each other -
 * folding an operand turns `x * 1` into an identity, an identity leaves a value nothing reads, and
 * removing that one can leave its operand unread in turn.
 *
 * The IR surgery used to be here too, and is now `IrEditor` in resolve/edit.h - reached from every
 * pass as `opt.ir()`. It moved because it is not about this stage: the resolver and the ownership
 * passes edit the same two-sided structures, and a rewrite that maintains only one side fails the
 * same way wherever it is written.
 */

// A cap on the fixed point rather than a trusted termination proof. Every rewrite here strictly
// reduces something - an operand becomes a constant, or an instruction goes away - so the loop
// terminates on its own; the cap is what turns a future pass that oscillates into a slow compile
// rather than a hang.
static constexpr Size kMaxRounds = 8;

/*
 * One pass, and the IR it left behind - see resolve/verify.h.
 *
 * A pass is the smallest thing that can be blamed, so this is the checkpoint the verifier exists for
 * and the reason the whole of it is written against a *stage* rather than one point in the pipeline.
 * Running it here also gives the "before" of every pass for free, since the check after one is the
 * check before the next; the round below opens with one for the first pass of the first round.
 *
 * Confined to assertion builds by `verifyIr`, because this is one full walk of a function per pass
 * per round - the fixed point runs the list up to eight times over every function in the program.
 */
#define runPass(pass) \
    pass(opt); \
    verifyIr(*opt.module, *opt.function, VerifyStage::Optimized, \
             (StringView { "after " #pass, sizeof("after " #pass) - 1 }))

void optimizeRounds(OptContext& opt) {
    for(Size round = 0; round < kMaxRounds; round++) {
        opt.changed = false;

        runPass(foldFunction);

        // Before the place passes rather than after them, because what it changes is which storage a
        // place names: a read left rooted in a borrow is one `forwardPlaces` cannot match against the
        // write that produced it, and one `computeContainment` refuses to call contained at all.
        runPass(collapseBorrows);

        runPass(forwardPlaces);
        runPass(scalarizeLocals);

        // After the scalarizer rather than before it: what promotion has to work on is one place per
        // field, and a record written whole is one place until `splitAggregateWrite` has taken it
        // apart. The removal that pays for this is the *next* round's - promotion leaves a local
        // whose whole use list is writes, which is the state `eliminateDeadLocal` removes it in.
        runPass(promotePlaces);

        // Immediately in front of the fold, because what it produces *is* a constant condition: a
        // loop it decides does nothing is one whose exit test it writes down as false, and every
        // consequence of that - the arm nothing reaches, the phi with one alternative left, the
        // header merged back into the block above it - belongs to the pass below.
        runPass(eliminateDeadLoops);

        // Immediately in front of the fold, for the reason the dead-loop pass is: what it produces is
        // a CFG the same cleanups have to run over. It is above the place passes' reach rather than
        // below them because what it removes is a *join*, and a join is where every block-local pass
        // stops - so the round after this one is the first that can see through a bounds check.
        runPass(endNonReturningBlocks);

        // After the place passes rather than before them, because most constant conditions are made
        // rather than written: a `Bool` inlining turned into a literal, a field forwarding answered
        // from the write above it. Ahead of the loop pass so that neither it nor the dominance walk
        // spends its time on blocks nothing reaches.
        runPass(foldBranches);

        /*
         * And the branches nothing folded, which are the ones that decide a value rather than a
         * path. After the fold rather than beside it, because the two want the diamond in opposite
         * states: a constant condition is a whole arm deleted, which is strictly better than a
         * select of both, so what reaches here is what that pass could not answer.
         *
         * It leaves one block where there were four, which is why it is above the block-local passes
         * rather than at the end of the round - a read and the write that answers it are only in one
         * block once the join has been merged back.
         */
        runPass(convertSelects);

        /*
         * A short-circuit condition left after if-conversion is a boolean phi followed by a branch.
         * Put that branch back on the incoming edges so a comparison which already decided an edge
         * can stay in its flags. This is deliberately *behind* convertSelects: a diamond that can be
         * one expression is better left to the JS structurizer as one select, while the joins that
         * survive it are control flow on every backend. See opt_branch.cpp.
         */
        runPass(threadBooleanBranches);

        // After forwarding rather than before it: a read the block-local pass already answered is
        // not a candidate, and one it could not answer is exactly what a loop keeps re-doing. Ahead
        // of CSE for the same reason in the other direction - two hoisted copies of one computation
        // land in the preheader together, where the dominator walk unifies them.
        runPass(hoistLoopValues);

        /*
         * Ahead of CSE rather than after it, because what it produces is a *second* spelling of a
         * value that already exists: the sign extension becomes a conversion of the zero extension,
         * and it is the dominator walk below that then unifies two subscripts of one index. Below
         * the branch folder for the reason every range question is - the arm has to still be there.
         */
        runPass(narrowCheckedIndexes);

        runPass(eliminateCommonValues);
        runPass(eliminateDeadValues);

        if(!opt.changed) break;
    }
}

namespace {

/*
 * Twice around, with the packing expansion in between.
 *
 * The order is the whole design. Above the expansion a place is structural, so two co-packed fields
 * are two pieces of storage and the aggregate passes can take a record apart; below it they are one
 * word, and what the second run has to work on is the arithmetic that says so. Running the same
 * passes on both sides is what turns nine read-modify-writes of one word into one.
 */
void optimizeFunction(OptContext& opt, Function& function) {
    opt.function = &function;

    // The lists as `flattenArguments` and `inlineCalls` left them, which is a question worth asking
    // here rather than a repair worth performing: both of those maintain them now, so a failure at
    // this checkpoint names the pass that broke one.
    verifyIr(*opt.module, function, VerifyStage::Ownership, "before optimizing"_v);

    optimizeRounds(opt);
    if(expandPacking(opt)) optimizeRounds(opt);

    verifyIr(*opt.module, function, VerifyStage::Optimized, "after optimizing"_v);
}

}

bool isConstructorIndex(OptContext& opt, TypePtr type) {
    if(!type || type == opt.program.scalar.bool_) return false;

    auto record = opt.global[type];
    if(record->kind != Type::Record) return false;

    return ((RecordType*)record)->layout == RecordType::Enum;
}

Maybe<IntFacts> foldableInt(OptContext& opt, TypePtr type) {
    if(!type) return Nothing();

    // `Bool` is an enum record rather than an integer type, and every operation on one - `xor b,
    // True` is what `!b` resolves to - is one-bit unsigned arithmetic. Both targets already agree
    // that its only values are zero and one, which is what makes it foldable at all.
    if(type == opt.program.scalar.bool_) return Just(IntFacts { 1, 1, false });

    if(opt.global[type]->kind != Type::Int) return Nothing();
    auto integer = (IntType*)opt.global[type];

    /*
     * A `@bits` refinement is declined, and the reason is a disagreement rather than a difficulty.
     *
     * The language rule is that arithmetic on a refinement happens at the *unrefined* type's width -
     * see narrowerThanRegister in resolve/lower.cpp, which excludes refinements for exactly that
     * reason - while `coerce` in codegen/js/type.cpp masks every integer to its own `bits`
     * whatever refined it. The two only agree today because dispatch types a refinement's arithmetic
     * at the type it refines, so no such instruction is ever built. Folding one would be picking a
     * side of a question the IR does not currently ask.
     */
    if(integer->canonical) return Nothing();
    if(integer->bits == 0 || integer->bits > 64) return Nothing();

    return Just(IntFacts {
        integer->bits, IntType::registerBits(integer->width), integer->isSigned
    });
}

U64 narrowToWidth(U64 value, const IntFacts& facts) {
    if(facts.bits >= 64) return value;

    auto mask = (U64(1) << facts.bits) - 1;
    auto masked = value & mask;

    // Sign-extended rather than masked, because that is the register a signed narrow value is held
    // in on native - `truncateToWidth` shifts up and arithmetically back down - and because
    // `constantValue` in codegen/js/place.cpp reads a constant back by sign-extending from `bits`.
    // One stored form satisfies both.
    if(facts.isSigned && (masked & (U64(1) << (facts.bits - 1)))) return masked | ~mask;

    return masked;
}

Maybe<U64> constantValueOf(OptContext& opt, ModulePtr<Value> value) {
    if(!value) return Nothing();

    auto constant = opt.local[value];
    if(constant->kind != Value::ConstInt) return Nothing();

    /*
     * A refinement's own facts where it has them, and the unrefined type's where `foldableInt`
     * declined - which for a *constant* is not the disagreement it declined over.
     *
     * That refusal is about results: what width `x + 1` wraps at on a `@bits(3) U32` is a question
     * the two targets answer differently, so nothing may fold one. Reading a literal is not that
     * question. The resolver produced a value already in range, and every fold computes in the
     * register the canonical type names - so this reads it there and the result is the same number
     * either way.
     */
    auto facts = foldableInt(opt, constant->type);
    if(!facts) facts = foldableInt(opt, canonicalType(opt.global, constant->type));
    if(facts) return Just(narrowToWidth(((ConstInt*)constant)->value, facts.unwrap()));

    /*
     * And a payload-free record, which *is* its constructor index - see `layoutRecord` in
     * repr/repr.cpp, where an enum's whole representation is a discriminant of however many bits its
     * constructor count needs.
     *
     * Read without any width at all, which is why this is here rather than an entry in `foldableInt`.
     * An index is a small non-negative number in every register that could hold it, so there is
     * nothing for a width to decide and nothing the two targets could disagree about; what
     * `foldableInt` would have to answer instead - which register an enum's *arithmetic* happens in
     * - is a repr question, and no arithmetic is typed at an enum in the first place. `Bool` is not
     * reached here: it has its own entry above, because a `Bool` is an operand of `xor` and does
     * need the width.
     *
     * What this is for is a `cast` of one to an integer, which is how `Ordering` leaves a `compare`
     * that folded: `cast EQ : Int` was the one link left unfolded in `Instance.yana` between the
     * comparison identity above and the `== 1` that reads its answer.
     */
    if(!isConstructorIndex(opt, constant->type)) return Nothing();

    return Just(((ConstInt*)constant)->value);
}

Maybe<FloatType::Width> foldableFloat(OptContext& opt, TypePtr type) {
    if(!type || opt.global[type]->kind != Type::Float) return Nothing();
    return Just(((FloatType*)opt.global[type])->width);
}

Maybe<F64> constantFloatOf(OptContext& opt, ModulePtr<Value> value) {
    if(!value) return Nothing();

    auto constant = opt.local[value];
    if(constant->kind == Value::ConstFloat) return Just(F64(((ConstFloat*)constant)->value));
    if(constant->kind == Value::ConstDouble) return Just(((ConstDouble*)constant)->value);

    return Nothing();
}

ModulePtr<Value> makeFloatConstant(OptContext& opt, Value& at, TypePtr type, F64 value) {
    auto width = foldableFloat(opt, type);
    assertTrue(width.isJust());

    auto block = opt.local[at.block];

    // The narrowing is the conversion, so it happens here rather than being left to whoever reads
    // the constant back: a `Float` holds an `F32` and `1.1` is not one of them.
    Value* constant = width.unwrap() == FloatType::Float
        ? (Value*)addConstant<ConstFloat>(*opt.module, *opt.function, *block, at.source, type, F32(value))
        : (Value*)addConstant<ConstDouble>(*opt.module, *opt.function, *block, at.source, type, value);

    return (ModulePtr<Value>)(constant - opt.local);
}

Maybe<Place> storageOf(OptContext& opt, ModulePtr<Value> value) {
    if(!value) return Nothing();

    if(opt.local[value]->kind == Value::LoadPlace) {
        return Just(((InstLoadPlace*)opt.local[value])->place);
    }

    for(U32 i = 0; i < opt.function->localCount(); i++) {
        if(opt.function->localAt(opt.local, i).value == value) return Just(Place::inLocal(i));
    }

    return Nothing();
}

// See opt_pass.h. Here rather than beside either caller because both of them ask it now: opt_arg.cpp
// has always projected fields out of the answer, and opt_inline.cpp used to ask `storageOf` for the
// root it re-bases a callee's places against - which refused every `return`-marked callee in the
// language, `elements` and `slice` and `get` among them, for handing over a borrow instead of a load.
Maybe<Place> argumentStorage(OptContext& opt, ModulePtr<Value> value) {
    if(value && opt.local[value]->kind == Value::Borrow) {
        return Just(((InstBorrow*)opt.local[value])->place);
    }

    return storageOf(opt, value);
}

Fields fieldsOf(OptContext& opt, TypePtr type) {
    if(!type) return {};

    Fields fields;
    auto content = type;

    if(opt.global[type]->kind == Type::Record) {
        auto record = (RecordType*)opt.global[type];

        // A sum has more than one shape and only one of them is live, which is a question about the
        // discriminant rather than about the path. An enum has no content at all.
        if(record->layout != RecordType::Single || record->constructors.isEmpty()) return {};

        auto constructor = record->constructors.get(opt.global, 0);

        // A boxed payload is one pointer the owner owns rather than a shape to be taken apart. See
        // the boxed-field case below, which is the same statement one level down.
        if(constructor.boxed) return {};

        fields.constructor = Just(U16(0));
        content = constructor.content;
    }

    if(!content || opt.global[content]->kind != Type::Tup) return {};

    /*
     * An aggregate with a boxed edge is not split into its fields.
     *
     * Everything built on this - argument splitting, aggregate scalarization - replaces one value by
     * its members and each member by an ordinary place. A boxed field's member is *not* an ordinary
     * place: reaching it is a load through a pointer the owner allocated and will free, so splitting
     * one would pass the box about as a value and leave two answers about who releases it.
     *
     * Declining costs the optimization on a type that has a box in it and nothing else. See
     * Field::boxed.
     */
    for(auto field: ((TupType*)opt.global[content])->fields.contents(opt.global)) {
        if(field.boxed) return {};
    }

    fields.content = content;
    fields.count = ((TupType*)opt.global[content])->fields.size();
    return fields.count ? fields : Fields {};
}

TypePtr fieldType(OptContext& opt, const Fields& fields, Size index) {
    return ((TupType*)opt.global[fields.content])->fields.get(opt.global, index).type;
}

StringId fieldName(OptContext& opt, const Fields& fields, Size index) {
    return ((TupType*)opt.global[fields.content])->fields.get(opt.global, index).name;
}

Place fieldPlace(OptContext& opt, Place base, const Fields& fields, U16 index) {
    Place result = base;

    // A fresh list rather than the one `base` holds, since several of these are built from one base
    // and a shared list would have every field appended to the same path.
    result.projections = {};
    for(Size i = 0; i < base.projections.size(); i++) {
        result.projections.push(opt.program.arena, base.projections.get(opt.local, i));
    }

    if(auto constructor = fields.constructor) {
        result.projections.push(opt.program.arena, Projection {
            ProjectionKind::Downcast, constructor.unwrap(), nullptr
        });
    }

    result.projections.push(opt.program.arena, Projection { ProjectionKind::Field, index, nullptr });
    return result;
}

ModulePtr<Value> makeConstant(OptContext& opt, Value& at, TypePtr type, U64 value) {
    auto block = opt.local[at.block];
    auto constant = addConstant<ConstInt>(*opt.module, *opt.function, *block, at.source, type, value);
    return (ModulePtr<Value>)(constant - opt.local);
}

void optimizeProgram(Context& context, Program& program, const ReprTarget& target) {
    // One target consumes one resolved program - `@platform` selects declarations during resolution,
    // so a JS build and a native build never share one - and this stage rewrites that program in
    // place. The flag is what says so out loud, rather than leaving a second call to be idempotent
    // by luck.
    if(program.optimized) return;
    program.optimized = true;

    // `-no-opt`, and the second half of the fixture runner's equivalence check. Marked as optimized
    // on the way past anyway, so that "this program has been through the stage" stays one question
    // with one answer rather than depending on what the stage decided to do.
    //
    // Still verified on the way out, at the stage the IR is actually at: the check below stands
    // between the IR and a backend, and switching this stage off does not make what a backend
    // assumes any weaker.
    if(!context.settings.optimizeIr) {
        verifyIrProgram(program, VerifyStage::Ownership, "before lowering"_v);
        return;
    }

    ReprTable repr(*program.types, target);
    OptContext opt { context, program, *program.types, *program.arena, repr };

    // What the resolver and the ownership passes left, before this stage touches it. There is no
    // longer anything special about this checkpoint - every pass below maintains the use lists, so
    // this is simply the first of them.
    verifyIrProgram(program, VerifyStage::Ownership, "on entry to the optimizer"_v);

    // Before the discharge, because what it reads is the drops as the ownership passes left them -
    // and the discharge is what turns those into calls. Answers which lambdas still need a closure
    // header emitted in front of them, which is a question no later stage can reconstruct.
    markClosureHeaders(opt);
    verifyIrProgram(program, VerifyStage::Ownership, "after markClosureHeaders"_v);

    // Over the whole program at once: it changes signatures, so it is the one thing here that a
    // single function's optimization cannot contain. What it leaves behind - a record rebuilt in
    // the callee, taken apart at the caller - is what the passes below remove.
    flattenArguments(opt);

    // A checkpoint that could not exist before this pass maintained its own use lists: it rewrote
    // signatures and call sites wholesale and left every list for a `rebuildUses`, so there was no
    // invariant here to ask about. It now drops and records the argument uses it moves, and retires
    // a parameter with `replaceValue`, which is exactly what this asks. Worth 4% of the assertion
    // build's fixture corpus, measured, for the two of them.
    verifyIrProgram(program, VerifyStage::Ownership, "after flattenArguments"_v);

    // Also program-wide, also before any function is optimized, and after flattening rather than
    // before it: a signature this stage is going to rewrite should be rewritten once, in the callee,
    // rather than once per copy of the callee. What inlining leaves behind is again work for the
    // passes below - a constructor's `alloc` in its caller's own block, which opt_scalar.cpp
    // removes, and arguments that are now constants, which opt_fold.cpp propagates.
    /*
     * The abort arms, ended - before the inliner rather than inside the rounds below, and the order
     * is the whole of what makes this item pay rather than cost.
     *
     * The fact this reads is a `Call` to a declared `noReturn` function, and inlining is what makes
     * that call stop existing: `checkFailed` is `exitProcess(134)`, so a caller that inlines it holds
     * a bare system call that nothing downstream can tell apart from any other. Cutting the edge
     * first leaves the block ending in `Unreachable`, and the body may then be copied in behind that
     * - which is what keeps the arm a *syscall* rather than a call, and §9.5's clobber set with it.
     *
     * Measured both ways round on the corpus: with the inliner declining instead, `Sieve` pays 7% for
     * the abort arm becoming an ordinary call in the middle of its loops.
     */
    for(auto module: program.modules) {
        opt.module = module;

        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];
            if(function->signature || function->blocks.isEmpty() || !function->used) continue;

            opt.function = function;
            endNonReturningBlocks(opt);
        }
    }

    verifyIrProgram(program, VerifyStage::Ownership, "after endNonReturningBlocks"_v);

    inlineCalls(opt);
    verifyIrProgram(program, VerifyStage::Optimized, "after inlineCalls"_v);

    /*
     * Now the ownership instructions become the calls they stand for - see dischargeOwnership.
     *
     * This used to be the first thing in the stage, because `clonableKind` refused a body holding
     * one and the discharge was what made those bodies inlinable. That is the wrong way round: the
     * inliner copies a whole body, so a drop copied with it runs once per call exactly as it did,
     * and admitting the three to `clonableKind` costs less than rewriting every body in front of a
     * pass that could have taken them as they were.
     *
     * What moving it buys is `reselectStorage`, which the inliner runs over each body it changed:
     * that re-derives where every allocation lives now that the call graph is the one that ships,
     * and it has to see a drop as a drop. The analysis has no case for `Drop` at all - which is
     * exactly why a literal built and dropped in one frame is frame-placed today - while a
     * discharged one is a call to a teardown that hands the pointer to `freeHeap`, and no summary
     * can tell that from a retention. Run in the old order it would decide nothing.
     *
     * Still before every per-function pass below, which is the constraint that was always the real
     * one: opt_select.cpp and the rest are written against a body whose teardowns are ordinary calls
     * they may move, fold and copy.
     */
    dischargeOwnership(opt);
    verifyIrProgram(program, VerifyStage::Optimized, "after dischargeOwnership"_v);

    /*
     * And the inliner again, over the teardowns that have just become ordinary calls.
     *
     * Not a second helping of the same pass: the two runs see different programs. The first sees
     * `Drop`, which it can copy but whose cost it can only estimate, and reaches it through
     * `inlineTeardown` - a path that was unreachable for non-generic bodies until the discharge
     * moved, and so has never been weighed against anything. The second sees the calls that drop
     * stands for, in the folded form `settle` leaves them in, and judges them through `inlineCall`
     * against a budget that ten programs' worth of measurement has already been spent on.
     *
     * Which is what the ordering costs and what this gives back. `Tree`'s teardown is a walk over a
     * recursive type and wants its two halves collapsed into one body; `Matrix`'s is a call in a
     * loop that wants to stay a call. The two are seven instructions and six, so no size rule tells
     * them apart - and the existing budget, handed the discharged forms, already gets both right.
     *
     * `reselectStorage` has run by now, so what this copies carries the storage classes the collapsed
     * call graph settled on rather than the ones the ownership stage guessed.
     */
    inlineCalls(opt);
    verifyIrProgram(program, VerifyStage::Optimized, "after the second inlineCalls"_v);

    for(auto module: program.modules) {
        opt.module = module;

        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];

            // A signature has no body by construction, and a body with no blocks is an intrinsic or
            // an unresolved declaration. A *generic* body is optimized like any other: it reaches
            // the backend whenever something took the erased path to it, and its specializations
            // were cloned long before this stage runs.
            if(function->signature || function->blocks.isEmpty()) continue;

            /*
             * And nothing outside the root module that the program cannot reach, which is the same
             * filter `lowerProgram` and `js::genProgram` apply before emitting: a body neither
             * backend will look at is one this stage can only spend time on.
             *
             * What that is worth is the whole of Core, Native and Collections. They are defined
             * from source on every compilation - around 490 bodies - and a program reaches between
             * one and thirty of them, so optimizing the rest was most of what this loop did.
             *
             * `used` is `resolveProgram`'s answer rather than a fresh one, and the two passes above
             * can only shrink the reachable set: inlining removes calls, and a body copied into its
             * caller names callees that were already reached through the callee. So the answer read
             * here is a superset of the truth, which is the safe direction - the cost is optimizing
             * a function that has since become unreachable, and never skipping one that is emitted.
             */
            if(!function->used) continue;

            optimizeFunction(opt, *function);
        }
    }

    /*
     * And which functions the program can still reach, which this stage is entitled to have changed.
     *
     * `resolveProgram` answered it once and inlining is what makes the answer stale: the `Call` that
     * named a callee is gone, and where it was the only one the body has no reason to be emitted.
     * Without this the inliner is pure growth - the callee copied into its caller *and* the callee -
     * which is what the measurement said before this line existed.
     *
     * Dead value elimination is the other producer: removing an unread `Symbol` removes the only
     * reference to a function reached by address. So this is at the end of the stage rather than at
     * the end of the pass that first needed it.
     */
    markProgramReachable(program);

    /*
     * And the last checkpoint - see resolve/verify.h.
     *
     * Unconditional rather than confined to assertion builds, unlike the per-pass checks above,
     * because this is the one that stands between the IR and a backend: everything below reads the
     * IR as a promise, and a promise broken here is a crash inside a code generator or a program
     * that computes the wrong number. One walk per program at the end of a stage that has already
     * walked every function eight times is not a cost worth trading that for.
     */
    verifyIrProgram(program, VerifyStage::Optimized, "before lowering"_v);
}
