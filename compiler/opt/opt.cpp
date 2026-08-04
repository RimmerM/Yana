#include "opt_pass.h"

/*
 * The driver, and the IR surgery every pass performs.
 *
 * Which passes run and how many times is here; what each of them decides lives in the file that
 * decides it. The loop is a fixed point rather than a sequence because the three feed each other -
 * folding an operand turns `x * 1` into an identity, an identity leaves a value nothing reads, and
 * removing that one can leave its operand unread in turn.
 */

namespace {


/*
 * Every value of one function, in no particular order: the parameters, the phis, the instructions
 * and each block's terminator.
 *
 * Constants are not among them and cannot be - one belongs to no block and is reached only through
 * whatever names it - so a caller that needs those too finds them by walking operands.
 */
template<class F>
void eachFunctionValue(OptContext& opt, F&& f) {
    for(auto arg: opt.function->args.contents(opt.local)) f((ModulePtr<Value>)arg);

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(auto phi: block->phis.contents(opt.local)) f((ModulePtr<Value>)phi);
        for(auto instruction: block->instructions.contents(opt.local)) f((ModulePtr<Value>)instruction);
        if(block->terminator) f((ModulePtr<Value>)block->terminator);
    }
}

/*
 * Recomputing every use list from the instructions that exist.
 *
 * Necessary rather than tidy, and the reason is a real gap: `Block::add` is what records a use, and
 * the drop pass does not go through it. `insertBlockDrops` and `splitEdge` in analyze_drop.cpp build
 * an `InstDrop` with `createInst` and push it straight into the block, so a drop is an instruction
 * that names a local and is in no use list - and a pass that asked "who touches this local" would be
 * told "only these writes" about storage that is also released.
 *
 * That was a crash rather than a subtlety: the dead-local rule below removed a heap allocation whose
 * `drop ... release` was still there, and the call to free it was left naming a value that no longer
 * existed. Repairing the lists once here is better than teaching each pass to distrust them, and it
 * also fixes the narrower version of the same gap - an overwrite drop's place may carry an index
 * value, which nothing had recorded a use of either.
 *
 * Two passes, because a list may only be cleared before anything is pushed into it.
 */

}

void rebuildUses(OptContext& opt) {
    auto forget = [&](ModulePtr<Value> value) {
        if(value) opt.local[value]->uses.clear();
    };

    eachFunctionValue(opt, [&](ModulePtr<Value> value) {
        forget(value);

        // Constants and arguments reached only from here, which is why this clears operands as well
        // as definitions rather than only the latter.
        eachOperand(opt.local, *opt.local[value], forget);
        eachRootValue(opt, *opt.local[value], forget);
    });

    eachFunctionValue(opt, [&](ModulePtr<Value> value) {
        auto user = (ModulePtr<Inst>)value;
        auto record = [&](ModulePtr<Value> operand) {
            opt.local[operand]->uses.push(opt.program.arena, user);
        };

        eachOperand(opt.local, *opt.local[value], record);
        eachRootValue(opt, *opt.local[value], record);
    });
}

// A cap on the fixed point rather than a trusted termination proof. Every rewrite here strictly
// reduces something - an operand becomes a constant, or an instruction goes away - so the loop
// terminates on its own; the cap is what turns a future pass that oscillates into a slow compile
// rather than a hang.
static constexpr Size kMaxRounds = 8;

void optimizeRounds(OptContext& opt) {
    for(Size round = 0; round < kMaxRounds; round++) {
        opt.changed = false;

        foldFunction(opt);

        // Before the place passes rather than after them, because what it changes is which storage a
        // place names: a read left rooted in a borrow is one `forwardPlaces` cannot match against the
        // write that produced it, and one `computeContainment` refuses to call contained at all.
        collapseBorrows(opt);

        forwardPlaces(opt);
        scalarizeLocals(opt);

        // After the scalarizer rather than before it: what promotion has to work on is one place per
        // field, and a record written whole is one place until `splitAggregateWrite` has taken it
        // apart. The removal that pays for this is the *next* round's - promotion leaves a local
        // whose whole use list is writes, which is the state `eliminateDeadLocal` removes it in.
        promotePlaces(opt);

        // Immediately in front of the fold, because what it produces *is* a constant condition: a
        // loop it decides does nothing is one whose exit test it writes down as false, and every
        // consequence of that - the arm nothing reaches, the phi with one alternative left, the
        // header merged back into the block above it - belongs to the pass below.
        eliminateDeadLoops(opt);

        // After the place passes rather than before them, because most constant conditions are made
        // rather than written: a `Bool` inlining turned into a literal, a field forwarding answered
        // from the write above it. Ahead of the loop pass so that neither it nor the dominance walk
        // spends its time on blocks nothing reaches.
        foldBranches(opt);

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
        convertSelects(opt);

        // After forwarding rather than before it: a read the block-local pass already answered is
        // not a candidate, and one it could not answer is exactly what a loop keeps re-doing. Ahead
        // of CSE for the same reason in the other direction - two hoisted copies of one computation
        // land in the preheader together, where the dominator walk unifies them.
        hoistLoopValues(opt);

        eliminateCommonValues(opt);
        eliminateDeadValues(opt);

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
    rebuildUses(opt);

    optimizeRounds(opt);
    if(expandPacking(opt)) optimizeRounds(opt);
}

}

void dropUse(OptContext& opt, ModulePtr<Value> value, ModulePtr<Inst> user) {
    if(!value) return;

    auto& uses = opt.local[value]->uses;
    for(Size i = 0; i < uses.size(); i++) {
        if(uses.get(opt.local, i) == user) {
            uses.remove(opt.local, i);
            return;
        }
    }
}

void replaceValue(OptContext& opt, ModulePtr<Value> from, ModulePtr<Value> to) {
    if(from == to) return;

    // Null would mean pointing a reader at nothing, which every caller is supposed to have already
    // decided against. Asserted rather than tolerated because the result is otherwise a use of
    // whatever sits at offset zero of the arena.
    assertTrue(to != nullptr);

    auto& uses = opt.local[from]->uses;
    while(uses.size()) {
        auto userPointer = uses.get(opt.local, uses.size() - 1);
        uses.remove(opt.local, uses.size() - 1);

        // Every matching operand at once, and one use entry per visit: an instruction reading the
        // value twice is in the list twice, so the two counts stay equal either way round.
        mapOperands(opt.local, *opt.local[userPointer], [&](ModulePtr<Value> operand) {
            return operand == from ? to : operand;
        });

        opt.local[to]->uses.push(opt.program.arena, userPointer);
    }

    opt.changed = true;
}

void eraseInstruction(OptContext& opt, ModulePtr<Inst> instruction) {
    auto value = opt.local[instruction];
    assertTrue(value->uses.isEmpty());

    eachOperand(opt.local, *value, [&](ModulePtr<Value> operand) {
        dropUse(opt, operand, instruction);
    });

    // The storage a place is rooted in, which `eachOperand` deliberately does not yield - see
    // eachRootValue. Missing it leaves the Alloc believing in a reader that is no longer in any
    // block, which is invisible until a pass asks the Alloc who reads it.
    eachRootValue(opt, *value, [&](ModulePtr<Value> storage) {
        dropUse(opt, storage, instruction);
    });

    auto block = opt.local[value->block];
    for(Size i = 0; i < block->instructions.size(); i++) {
        if(block->instructions.get(opt.local, i) == instruction) {
            block->instructions.remove(opt.local, i);
            break;
        }
    }

    opt.changed = true;
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

void insertInstructions(OptContext& opt, Block& block, Size index, InstList& instructions) {
    /*
     * Registered through `Block::add` rather than written into the list directly, because `add` is
     * what records every use - a use list a pass filled in by hand would be one more place for the
     * two directions of the IR to disagree. It appends, so the list is rebuilt afterwards with the
     * new instructions moved to where they were wanted.
     */
    auto existing = block.instructions.size();
    for(auto instruction: instructions) block.add(*opt.module, instruction);

    SmallArray<ModulePtr<Inst>, 48> ordered;
    for(Size i = 0; i < existing; i++) {
        if(i == index) {
            for(auto j = existing; j < block.instructions.size(); j++) {
                ordered.push(block.instructions.get(opt.local, j));
            }
        }

        ordered.push(block.instructions.get(opt.local, i));
    }

    // An index at the end of the list, which nothing above would have reached.
    if(index >= existing) {
        for(auto j = existing; j < block.instructions.size(); j++) {
            ordered.push(block.instructions.get(opt.local, j));
        }
    }

    block.instructions.clear();
    for(auto instruction: ordered) block.instructions.push(opt.program.arena, instruction);

    opt.changed = true;
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
    if(!context.settings.optimizeIr) return;

    ReprTable repr(*program.types, target);
    OptContext opt { context, program, *program.types, *program.arena, repr };

    // Before the discharge, because what it reads is the drops as the ownership passes left them -
    // and the discharge is what turns those into calls. Answers which lambdas still need a closure
    // header emitted in front of them, which is a question no later stage can reconstruct.
    markClosureHeaders(opt);

    // Before anything else here, because everything else here is written under the constraint it
    // removes - see dischargeOwnership. What it leaves behind is ordinary calls, which the passes
    // below are entitled to move, fold and copy like any other.
    dischargeOwnership(opt);

    // Over the whole program at once: it changes signatures, so it is the one thing here that a
    // single function's optimization cannot contain. What it leaves behind - a record rebuilt in
    // the callee, taken apart at the caller - is what the passes below remove.
    flattenArguments(opt);

    // Also program-wide, also before any function is optimized, and after flattening rather than
    // before it: a signature this stage is going to rewrite should be rewritten once, in the callee,
    // rather than once per copy of the callee. What inlining leaves behind is again work for the
    // passes below - a constructor's `alloc` in its caller's own block, which opt_scalar.cpp
    // removes, and arguments that are now constants, which opt_fold.cpp propagates.
    inlineCalls(opt);

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
}
