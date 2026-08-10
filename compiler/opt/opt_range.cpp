#include "opt_pass.h"

/*
 * What a branch proves about the values below it, and the one rewrite that wants it.
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
 * `isNonNegative32` in codegen/x64/transform.cpp is that question asked of a definition's kind, and
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
 * ## What it is not
 *
 * Not a range analysis. There is no lattice, no fixpoint and no per-block state: one branch is read,
 * one arm is looked at, one fact is used. A real one would carry intervals down the dominator tree
 * and answer questions this cannot - whether the second bounds test of an already-checked index is
 * redundant, which is the item §9 left. The reason to write this one first is that it is the only
 * consumer that exists, and inventing the lattice before the consumer is how a range analysis ends
 * up shaped for nothing in particular.
 */

namespace {

// The widest value a type can hold, as far as this pass cares - and only for the unsigned ones,
// which is the only direction the bound is wanted in. A `@bits` refinement is read rather than
// declined, unlike `foldableInt`: what that function refuses is *arithmetic* at a refined width,
// and this is a statement about the values rather than about an operation.
Maybe<U16> unsignedBitsOf(OptContext& opt, TypePtr type) {
    if(!type || opt.global[type]->kind != Type::Int) return Nothing();

    auto integer = (IntType*)opt.global[type];
    if(integer->isSigned || integer->bits == 0 || integer->bits > 64) return Nothing();

    return Just(integer->bits);
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
