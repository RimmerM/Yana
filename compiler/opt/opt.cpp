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

// A cap on the fixed point rather than a trusted termination proof. Every rewrite here strictly
// reduces something - an operand becomes a constant, or an instruction goes away - so the loop
// terminates on its own; the cap is what turns a future pass that oscillates into a slow compile
// rather than a hang.
constexpr Size kMaxRounds = 8;

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

void optimizeFunction(OptContext& opt, Function& function) {
    opt.function = &function;
    rebuildUses(opt);

    for(Size round = 0; round < kMaxRounds; round++) {
        opt.changed = false;

        foldFunction(opt);
        forwardPlaces(opt);
        scalarizeLocals(opt);
        eliminateCommonValues(opt);
        eliminateDeadValues(opt);

        if(!opt.changed) break;
    }
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

    auto facts = foldableInt(opt, constant->type);
    if(!facts) return Nothing();

    return Just(narrowToWidth(((ConstInt*)constant)->value, facts.unwrap()));
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

    for(auto module: program.modules) {
        opt.module = module;

        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];

            // A signature has no body by construction, and a body with no blocks is an intrinsic or
            // an unresolved declaration. A *generic* body is optimized like any other: it reaches
            // the backend whenever something took the erased path to it, and its specializations
            // were cloned long before this stage runs.
            if(function->signature || function->blocks.isEmpty()) continue;

            optimizeFunction(opt, *function);
        }
    }
}
