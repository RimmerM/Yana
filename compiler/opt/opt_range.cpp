#include "opt_pass.h"

/*
 * What a branch proves about the values below it, and the two rewrites that want it.
 *
 * Every other fold in this directory reasons about the *operands* an instruction carries - two
 * constants, a reflexive comparison, a shift by a known amount. This one reasons about the edge:
 * `je %c, abort, ok` says nothing about `%c` where it stands, and says everything about it on each
 * of the two blocks it leads to. §11.5 of test/bench/findings.md is where the want came from, and it
 * names the same machinery for the two other things that would read it - the bounds-check folding
 * §9 left half-done, and any range analysis after this.
 *
 * ## The shape, which is every checked subscript in the language
 *
 * A subscript emits two widenings of one index and a test between them:
 *
 *     %i64 = cast %x : I64          -- sign-extended, because `Int` is signed; the address wants it
 *     %u64 = cast %x : U64          -- zero-extended; the check wants it, since a negative index
 *     %ok  = cmp_ge %u64, %length   -- has to compare *above* every length rather than below zero
 *     je %ok, abort, body
 *
 * Both survive to the machine, and they are the same register whenever `%x` has no bit 31 -
 * `isNonNegative32` in codegen/x64/transform_peephole.cpp is that question asked of a definition's kind, and
 * §11.5 landed the half of it that a definition can answer for itself. This is the other half, and
 * it is not a property of `%x` at all: what says bit 31 is clear is the *branch*, on the arm where
 * `%u64 <u %length` and `%length` is a `@bits(30)` field. Neither half of that survives lowering -
 * below the fork the refinement is gone and the two casts are both `Long`.
 *
 * ## What is rewritten, and why that direction
 *
 * The sign extension's *source*, not its readers and not the comparison. `cast %u64 : I64` is a
 * 64-to-64 conversion, which `trySkipCastExtend` already emits nothing for, so the `movsxd`
 * disappears and the zero extension the check needed is the one register both uses share. Rewriting
 * the other way round is not available: `lowerCmp` reads the comparison's signedness off its left
 * operand's type, so putting a signed value there would turn the bounds test into a signed one -
 * and a signed test is exactly what a negative index defeats.
 *
 * The sign extension is *rebuilt at the top of the arm* rather than edited where it stands. Its
 * block is the one holding the branch, where the fact is not yet true - `%x` may well be negative on
 * the path that aborts - so the fact has to be used strictly below the edge that proves it. Every
 * reader has to be dominated by that arm for this to be a replacement at all, which the walk checks
 * rather than assumes.
 *
 * ## The second rewrite: a zero test something above it already decided
 *
 * `foldProvenZeroTests`, and it is the same machinery pointed at a different fact. A division emits
 * `checkCondition(d == 0)` (see the ruling beside `Div` in resolve/inst.def), the inliner turns that
 * into a branch, and two shapes then ask the identical question twice:
 *
 *     if d != 0 then x / d else 0      -- the guard proves it, the check asks anyway
 *     x / d + y / d                    -- the first check proves it for the second
 *
 * Both are one rule. A comparison of `%v` against zero is answered `false` (or `true`, written the
 * other way up) wherever some *other* comparison of the same `%v` against zero branches, and the arm
 * of that branch on which `%v` is not zero dominates this one. The two shapes differ only in which
 * arm the proof came from - the `else` of an `Eq` and the `then` of a `Ne` - which is why neither
 * needs a case of its own.
 *
 * What it deliberately does not do is remove the *check*. It folds the condition and stops; the
 * branch folder above it retires the branch on the next round, and the dead arm and its call go with
 * `eliminateDeadValues`. That keeps this pass to the one thing it can prove, and means an
 * uninlined `checkCondition` gets a constant argument rather than a special case here.
 *
 * ## What it is not
 *
 * Still not a range analysis. There is no lattice, no fixpoint and no per-block state: a fact is
 * found by looking at the *other readers of one value*, which is why two rewrites can share the
 * machinery without either of them carrying intervals down the dominator tree. A real one would
 * answer questions neither can - whether the second bounds test of an already-checked index is
 * redundant, which is the item §9 left, or whether a divisor is non-zero because it is a length.
 * Both rewrites here are the cases that fall out of one comparison, and the reason to write them
 * this way first is that inventing the lattice before the consumers is how a range analysis ends up
 * shaped for nothing in particular.
 */

namespace {

// The widest value a type can hold, as far as this pass cares - and only for the unsigned ones,
// which is the only direction the bound is wanted in. A `@bits` refinement is read rather than
// declined, unlike `foldableInt`: what that function refuses is *arithmetic* at a refined width,
// and this is a statement about the values rather than about an operation.
Maybe<U16> unsignedBitsOf(OptContext& opt, TypePtr type) {
    if(!type || opt.global[type]->kind != Type::Int) return Nothing();

    auto integer = (IntType*)opt.global[type];
    auto bits = integer->bitsOn(opt.repr.target.integers);
    if(integer->isSigned || bits == 0 || bits > 64) return Nothing();

    return Just(bits);
}

/*
 * Whether a value is known to be below 2^31, which is the bound the rewrite needs of a length.
 *
 * Two ways to know. A constant says so outright, and a chain of widening casts says so if what it
 * started from does: `cast (cast (load %xs@Array.length : @bits(30) U32) : U32) : U64` is how a
 * container's own length reaches a bounds test, and only the innermost of the three carries the
 * refinement that decides it.
 *
 * Every step of the walk has to be *value-preserving*, which for an integer cast means unsigned and
 * no narrower. A signed source may sign-extend, which turns a small negative into a huge unsigned;
 * a narrower target truncates. Neither is a step this may take, and neither arises on the chain the
 * subscript builds.
 */
bool fitsBelowSignBit(OptContext& opt, ModulePtr<Value> value) {
    for(Size step = 0; step < 8; step++) {
        auto& instruction = *opt.local[value];

        if(auto number = constantValueOf(opt, value)) {
            return number.unwrap() < (U64(1) << 31);
        }

        if(auto bits = unsignedBitsOf(opt, instruction.type)) {
            if(bits.unwrap() <= 31) return true;
        }

        if(instruction.kind != Value::Cast) return false;

        auto source = ((InstUnary&)instruction).from;
        if(!source) return false;

        auto sourceBits = unsignedBitsOf(opt, opt.local[source]->type);
        auto targetBits = unsignedBitsOf(opt, instruction.type);
        if(!sourceBits || !targetBits) return false;
        if(sourceBits.unwrap() > targetBits.unwrap()) return false;

        value = source;
    }

    return false;
}

// The zero extension a bounds test compares: a cast of a 32-bit *signed* integer to an unsigned
// 64-bit one, which is where the two spellings of one index diverge. Answers the source, or null.
ModulePtr<Value> zeroExtendedIndex(OptContext& opt, ModulePtr<Value> value) {
    auto& instruction = *opt.local[value];
    if(instruction.kind != Value::Cast) return nullptr;

    auto target = foldableInt(opt, instruction.type);
    if(!target || target.unwrap().isSigned || target.unwrap().bits != 64) return nullptr;

    auto source = ((InstUnary&)instruction).from;
    if(!source) return nullptr;

    auto from = foldableInt(opt, opt.local[source]->type);
    if(!from || !from.unwrap().isSigned || from.unwrap().registerBits != 32) return nullptr;

    return source;
}

// The matching sign extension: the same source widened to a signed 64-bit type, which is what the
// address arithmetic reads. There is at most one worth finding, and finding none is the ordinary
// answer for a subscript whose element address was folded away.
ModulePtr<Value> signExtensionOf(OptContext& opt, ModulePtr<Value> index) {
    for(auto user: opt.local[index]->uses(opt.local)) {
        auto& instruction = *opt.local[user];
        if(instruction.kind != Value::Cast) continue;
        if(((InstUnary&)instruction).from != index) continue;

        auto target = foldableInt(opt, instruction.type);
        if(!target || !target.unwrap().isSigned || target.unwrap().bits != 64) continue;

        return (ModulePtr<Value>)user;
    }

    return nullptr;
}

/*
 * Whether every reader of a value sits strictly below a block.
 *
 * A phi is refused outright rather than answered by its incoming block. Its operand is read on an
 * *edge* rather than in a block, so "dominated by the arm" is a question about the edge - and a
 * definition that reaches the phi over some other edge would be replaced along with it.
 */
bool usesBelow(OptContext& opt, Dominance& dominance, ModulePtr<Value> value, U32 arm) {
    for(auto user: opt.local[value]->uses(opt.local)) {
        auto& instruction = *opt.local[user];
        if(instruction.kind == Value::Phi) return false;
        if(!instruction.block) return false;

        auto index = opt.local[instruction.block]->index;
        if(index >= dominance.dominators.size()) return false;
        if(!dominance.dominators[index][arm]) return false;
    }

    return true;
}

/*
 * The arm of a branch on which `%a <u %b` holds, or nothing.
 *
 * Only the two orderings a bounds test is spelled as. `cmp_ge` is what the resolver emits - the
 * abort is the *then* arm - and `cmp_lt` is the same test written the other way up, which is what a
 * source-level guard produces. `Le` and `Gt` would give `a <= b`, which is a weaker fact than this
 * needs and one no shape here asks for.
 */
/*
 * The value a comparison tests against a literal zero, or null - either operand order, and only the
 * two comparisons that decide it outright.
 *
 * `Lt` and the rest are deliberately absent even though `%v <u 0` is decidable: they are not what
 * either shape writes, and a comparison this pass answers is one it also has to know the *sense* of.
 * Integers only, for the same reason `foldableInt` exists - what a refinement does with a value is a
 * question the targets answer together only for the comparison, not for what led to it.
 */
ModulePtr<Value> zeroTestOperand(OptContext& opt, Value& instruction, bool& testsEqual) {
    if(instruction.kind != Value::Cmp) return nullptr;

    auto& compare = (InstCmp&)instruction;
    if(compare.cmp != CompareOp::Eq && compare.cmp != CompareOp::Ne) return nullptr;

    testsEqual = compare.cmp == CompareOp::Eq;

    auto left = constantValueOf(opt, compare.lhs);
    auto right = constantValueOf(opt, compare.rhs);

    // Exactly one side a literal zero. Two literals is a fold `foldFunction` already owns, and
    // answering it here would report a change on every round of the fixed point.
    auto tested = ModulePtr<Value>(nullptr);
    if(!left && right && right.unwrap() == 0) tested = compare.lhs;
    else if(!right && left && left.unwrap() == 0) tested = compare.rhs;

    if(!tested || !foldableInt(opt, opt.local[tested]->type)) return nullptr;

    return tested;
}

/*
 * The arm of the branch this comparison decides on which its operand is *not* zero, or null.
 *
 * The comparison has to be the branch's own condition rather than merely sit in the same block: a
 * value computed above a branch that reads something else is proof of nothing below it.
 */
ModulePtr<Block> nonZeroArm(OptContext& opt, ModulePtr<Value> comparison, bool testsEqual) {
    auto owner = opt.local[opt.local[comparison]->block];
    if(!owner || !owner->terminator()) return nullptr;

    auto& terminator = *opt.local[owner->terminator()];
    if(terminator.kind != Value::Je) return nullptr;

    auto& branch = (InstJe&)terminator;
    if(branch.cond != comparison) return nullptr;

    return testsEqual ? branch.elseBlock : branch.thenBlock;
}

/*
 * Whether something above this block has already decided that `tested` is not zero.
 *
 * Found through the *use list* of the value rather than by walking the dominator tree, which is what
 * keeps this to the "no per-block state" rule the header claims: a value is compared against zero
 * two or three times in the programs this fires on, and every candidate proof is one of those.
 *
 * `asking` is excluded because a comparison is not its own proof - and the exclusion has to be by
 * identity rather than by block, since the two shapes this exists for put the proof and the question
 * in different blocks in one case and the same value in neither.
 */
bool provenNonZero(OptContext& opt, Dominance& dominance, ModulePtr<Value> tested,
                   ModulePtr<Value> asking, U32 at) {
    if(at >= dominance.dominators.size()) return false;

    for(auto user: opt.local[tested]->uses(opt.local)) {
        auto candidate = (ModulePtr<Value>)user;
        if(candidate == asking) continue;

        auto testsEqual = false;
        if(zeroTestOperand(opt, *opt.local[candidate], testsEqual) != tested) continue;

        auto armPointer = nonZeroArm(opt, candidate, testsEqual);
        if(!armPointer) continue;

        auto arm = opt.local[armPointer];

        /*
         * The fact belongs to the edge, so it may only be used where the edge is the one way in -
         * `narrowCheckedIndexes` refuses a second predecessor for the identical reason. An arm that
         * is the branch's own block is a self-loop, whose top is above the comparison rather than
         * below it.
         */
        if(armPointer == opt.local[candidate]->block) continue;
        if(arm->incoming(opt.local).size() != 1) continue;

        auto index = arm->index;
        if(index >= dominance.dominators.size()) continue;
        if(dominance.dominators[at][index]) return true;
    }

    return false;
}

ModulePtr<Block> armBelow(OptContext& opt, InstCmp& compare, InstJe& branch) {
    auto facts = foldableInt(opt, opt.local[compare.lhs]->type);
    if(!facts || facts.unwrap().isSigned) return nullptr;

    if(compare.cmp == CompareOp::Ge) return branch.elseBlock;
    if(compare.cmp == CompareOp::Lt) return branch.thenBlock;

    return nullptr;
}

}

/*
 * One walk, and it rewrites nothing it cannot see the whole of.
 *
 * Written over the block list rather than over the dominator tree because there is no state to
 * carry: each branch is answered from its own two operands, and the only thing the tree is asked is
 * whether a reader sits below an arm.
 */
void narrowCheckedIndexes(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    auto& dominance = dominanceOf(opt);

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];
        if(!block->terminator()) continue;

        auto& terminator = *opt.local[block->terminator()];
        if(terminator.kind != Value::Je) continue;

        auto& branch = (InstJe&)terminator;
        if(!branch.cond) continue;

        auto& condition = *opt.local[branch.cond];
        if(condition.kind != Value::Cmp) continue;

        auto& compare = (InstCmp&)condition;
        auto below = armBelow(opt, compare, branch);
        if(!below) continue;

        auto arm = opt.local[below];

        /*
         * The fact belongs to the edge, so it may only be used where the edge is the one way in. A
         * second predecessor is a path the comparison was never made on, and an arm that *is* the
         * branch's own block is a self-loop whose top - where the rewrite goes - is above the
         * comparison rather than below it.
         */
        if(arm == block) continue;
        if(arm->incoming(opt.local).size() != 1) continue;

        auto index = zeroExtendedIndex(opt, compare.lhs);
        if(!index) continue;
        if(!fitsBelowSignBit(opt, compare.rhs)) continue;

        auto extension = signExtensionOf(opt, index);
        if(!extension) continue;

        // Nothing to replace is nothing to do, and saying otherwise would report a change on every
        // round of the fixed point for a dead value `eliminateDeadValues` is about to remove anyway.
        if(opt.local[extension]->useCount() == 0) continue;
        if(!usesBelow(opt, dominance, extension, arm->index)) continue;

        /*
         * The same widening said again from the value that already holds it. `%x` has no bit 31 on
         * this arm, so the zero extension and the sign extension are one number - and this spelling
         * of it is a conversion between two 64-bit types, which the backend emits nothing for.
         */
        auto replacement = createInst<InstUnary>(
            *opt.module, *opt.function, *arm, opt.local[extension]->source, StringId(),
            opt.local[extension]->type, Value::Cast, compare.lhs);

        InstList inserted;
        inserted.push(replacement);
        opt.ir().insert(*arm, 0, inserted);

        auto value = (ModulePtr<Value>)(replacement - opt.local);
        opt.ir().replaceValue(extension, value);
        opt.changed = true;
    }
}

/*
 * The second walk - see the header. One pass over the instructions, and the only state is the
 * dominator matrix the walk above already built.
 */
void foldProvenZeroTests(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    auto& dominance = dominanceOf(opt);

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(auto instructionPointer: block->instructions(opt.local)) {
            auto value = (ModulePtr<Value>)instructionPointer;
            auto& instruction = *opt.local[value];

            auto testsEqual = false;
            auto tested = zeroTestOperand(opt, instruction, testsEqual);
            if(!tested) continue;

            // Nothing reading it is nothing to answer, and saying otherwise would report a change on
            // every round for a value `eliminateDeadValues` is about to take anyway.
            if(instruction.useCount() == 0) continue;
            if(!provenNonZero(opt, dominance, tested, value, block->index)) continue;

            // `%v == 0` is false where `%v` is known not to be zero, and `%v != 0` is true. The
            // constant is built at the comparison's own type, which is `Bool` for both.
            auto answer = makeConstant(opt, instruction, instruction.type, testsEqual ? 0 : 1);
            opt.ir().replaceValue(value, answer);
            opt.changed = true;
        }
    }
}
