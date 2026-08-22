#include "opt_pass.h"

/*
 * A `Bool` select is a boolean function of two truth values, and every one of those is bitwise.
 *
 * `a && b` is a diamond in the resolve IR - the right operand under a branch, and a phi at the join
 * whose skipped alternative is the constant the operator settles on. `convertSelects` turns that
 * into `select c ? b : False`, and what reaches the backends is a conditional move:
 *
 *     mov  edi, 0            ; the constant arm, which a cmov cannot carry as an immediate
 *     test eax, eax
 *     cmove ecx, edi
 *
 * Three instructions and ten bytes for `and ecx, eax`. The two arms of a `Bool` select are 0 or 1
 * by the type's own definition, so a select between them is `and`, `or` or `xor` of values the
 * function already has - and those are instructions `Bitwise(Bool)` already declares, `lower_calc`
 * already lowers, and both backends already emit (`genBinary` in codegen/js reads a `Bool`'s three
 * bitwise operations as `&`, `|` and `^` for the same reason this pass exists).
 *
 * ## Why this is not the folder
 *
 * `foldSelect` in opt_fold.cpp answers a select with a value that is already there - an arm, the
 * condition, or a constant. **Every rule below needs an instruction that does not exist yet**, and
 * that pass deliberately inserts none: its walk is an index over a list nothing is added to, which
 * is the property that lets it run first in the round. So this is its own walk, over the one
 * instruction kind it rewrites, and the division between the two files is exactly that - whether
 * answering costs an instruction.
 *
 * `c ? True : False` is therefore *not* here. It is `c`, a value already standing there, so it is
 * one of `foldSelect`'s and is answered a pass earlier than this one runs.
 *
 * ## And the other half of the algebra, which is De Morgan
 *
 * `!a || !b` is `!(a && b)`, and the reason that is worth a rule is that the left-hand side is what
 * the language *emits*: `!a` is `xor a, True` - the one spelling of a `Bool`'s complement in this IR,
 * see the ruling in resolve/core.cpp - and the short circuit between them becomes the `or` the rule
 * above builds. So three instructions arrive here where two say the same thing:
 *
 *     %n = xor %a, True                    %t = and %a, %b
 *     %m = xor %b, True        becomes     %r = xor %t, True
 *     %r = or %n, %m
 *
 * and the second form is better than one instruction's worth, because of where these end up. A
 * complement on the *outside* is one `negatedBranchCondition` in lower/lower_fold.cpp can answer:
 * where the result feeds a branch, it swaps the two arms and the `xor` goes as well, so the three
 * instructions become one. Written the first way there is nothing for it to see - an `or` is not a
 * complement of anything.
 *
 * Gated on **both** complements having a single reader, which is what makes it a shrink rather than
 * a reshuffle: where `!a` is wanted elsewhere it has to stay, and the rewrite would then be four
 * instructions where there were three.
 *
 * The two spellings are kept apart rather than mixed. `Bitwise(Bool).not` is `xor c, True` and every
 * wider width is `Value::Not`, so a pair that agrees is rebuilt as what it was - which is also what
 * keeps this clear of the question `foldUnaryValue` documents beside `Value::Not`, where an unsigned
 * type narrower than its register makes the two targets disagree about what a complement *is*. This
 * introduces no complement of a kind or a type that was not already standing there twice.
 *
 * ## Why the mirror pair is declined
 *
 * The four rules are the ones where a select becomes *one* instruction or none. Their mirror images
 * - a false arm of `True`, a true arm of `False` - are the same rewrite over `!c`, and `!c` is a
 * second instruction this would have to insert. That is a trade about whether the negation is free,
 * which is a question about the rest of the function rather than about this instruction: it is free
 * where the program already computed `!c` and costs a `xor` and possibly a copy where it did not.
 * Nothing here can see that, so the shapes that need it are left as a `cmov`, which is what a
 * conditional move is for.
 *
 * ## The widened boolean is somewhere else
 *
 * `if c then 1 else 0` is a `Bool` widened to an `Int` and not a boolean operation, so it is not
 * one of these rules. It does not survive either: `Bool` and `Int` are the same lower type, and
 * `foldBooleanValue` in lower/lower_fold.cpp forwards `select %c, 1, 0` to `%c` for a `%c` that is
 * already 0 or 1 - which, since this pass runs, now includes a chain of these.
 */

namespace {

struct BooleanReducer {
    OptContext& opt;
    TypePtr bool_ = nullptr;

    ModulePtr<Value> valueOf(Inst* instruction) {
        return (ModulePtr<Value>)(instruction - opt.local);
    }

    Inst* binary(Block& block, LocationId source, StringId name, Value::Kind kind,
                 ModulePtr<Value> lhs, ModulePtr<Value> rhs) {
        return createInst<InstBinary>(*opt.module, *opt.function, block, source, name, bool_,
                                      kind, lhs, rhs);
    }

    /*
     * One select, answered as either a value that already exists or a single instruction to put in
     * its place.
     *
     * The condition is checked against `Bool` as well as the result, because `Select` is also the
     * lane-wise one: a vector select's condition is a `Mask`, its arms are vectors, and neither the
     * arms being 0 or 1 nor `and` meaning what it means here is true of it.
     */
    bool rewriteSelect(Block& block, Size index, ModulePtr<Inst> pointer) {
        auto& select = (InstSelect&)*opt.local[pointer];
        if(select.type != bool_) return false;
        if(opt.local[select.cond]->type != bool_) return false;

        auto whenTrue = constantValueOf(opt, select.whenTrue);
        auto whenFalse = constantValueOf(opt, select.whenFalse);

        // Two constants that are not the same one, and not `c` itself either: the equal pair and the
        // identity `c ? True : False` are both `foldSelect`'s, which runs first in the round and
        // answers each of them with a value that is already there. What is left for this pass is the
        // complement, which is an instruction.
        if(whenTrue && whenFalse) {
            if(whenTrue.unwrap() != 0 || whenFalse.unwrap() != 1) return false;

            // `!c`, which is `xor c, True` - the same single instruction `emitLogicalNot` produces,
            // and the one spelling of a `Bool`'s complement in this IR.
            auto one = makeConstant(opt, *opt.local[pointer], bool_, 1);
            auto replacement = binary(block, select.source, select.name, Value::Xor, select.cond, one);
            return replace(block, index, pointer, replacement);
        }

        /*
         * And the two the short circuit leaves. `c && b` answers `b` where `c` held and `False`
         * where it did not, which is `c & b`; `c || b` is the same statement about `or`. Neither
         * needs the arm to be a constant on the other side - what makes the rewrite sound is that
         * both arms are `Bool`, so both are already 0 or 1.
         */
        if(whenFalse && whenFalse.unwrap() == 0) {
            auto replacement = binary(block, select.source, select.name, Value::And,
                                      select.cond, select.whenTrue);
            return replace(block, index, pointer, replacement);
        }

        if(whenTrue && whenTrue.unwrap() == 1) {
            auto replacement = binary(block, select.source, select.name, Value::Or,
                                      select.cond, select.whenFalse);
            return replace(block, index, pointer, replacement);
        }

        return false;
    }

    /*
     * What an instruction complements, and how it spells it - or nothing where it complements
     * nothing.
     *
     * `Value::Not` is the complement at every width the language has one for. `xor c, True` is a
     * `Bool`'s, and is a complement only *because* the type is one bit: at any wider one, `xor x, 1`
     * flips the lowest bit and leaves the rest, which is not this operation at all. So the `Bool`
     * test is the rule rather than a shortcut to it.
     */
    ModulePtr<Value> complementOf(ModulePtr<Value> value, Value::Kind& spelling) {
        auto& instruction = *opt.local[value];

        if(instruction.kind == Value::Not) {
            spelling = Value::Not;
            return ((InstUnary&)instruction).from;
        }

        if(instruction.kind != Value::Xor || instruction.type != bool_) return nullptr;

        auto& binary = (InstBinary&)instruction;
        auto constant = constantValueOf(opt, binary.rhs);
        if(!constant || constant.unwrap() != 1) return nullptr;

        spelling = Value::Xor;
        return binary.lhs;
    }

    // De Morgan - see the file comment. Two complements combined are one complement of the
    // combination, which is one instruction fewer and puts it where a branch can use it.
    bool rewriteDeMorgan(Block& block, Size index, ModulePtr<Inst> pointer) {
        auto& instruction = *opt.local[pointer];
        if(instruction.kind != Value::And && instruction.kind != Value::Or) return false;

        auto& binary = (InstBinary&)instruction;

        // Both read once, which is what makes this a shrink; and the same spelling, so that what is
        // rebuilt is what was taken apart.
        if(opt.local[binary.lhs]->useCount() != 1) return false;
        if(opt.local[binary.rhs]->useCount() != 1) return false;

        auto leftSpelling = Value::Not;
        auto rightSpelling = Value::Not;

        auto left = complementOf(binary.lhs, leftSpelling);
        auto right = complementOf(binary.rhs, rightSpelling);
        if(!left || !right || leftSpelling != rightSpelling) return false;

        // One type throughout, which a complement and its operand always share and which the two
        // sides of the combination have to as well - `binary` may be at a type neither is.
        auto type = instruction.type;
        if(opt.local[left]->type != type || opt.local[right]->type != type) return false;

        auto combined = instruction.kind == Value::And ? Value::Or : Value::And;
        auto inner = createInst<InstBinary>(*opt.module, *opt.function, block, instruction.source,
                                            StringId(), type, combined, left, right);

        auto innerValue = valueOf(inner);
        Inst* outer;

        if(leftSpelling == Value::Not) {
            outer = createInst<InstUnary>(*opt.module, *opt.function, block, instruction.source,
                                          instruction.name, type, Value::Not, innerValue);
        } else {
            auto one = makeConstant(opt, instruction, bool_, 1);
            outer = createInst<InstBinary>(*opt.module, *opt.function, block, instruction.source,
                                           instruction.name, bool_, Value::Xor, innerValue, one);
        }

        InstList instructions;
        instructions.push(inner);
        instructions.push(outer);

        return replaceWith(block, index, pointer, instructions, valueOf(outer));
    }

    // The replacement put where the select was, its readers pointed at it, and the select erased.
    // In that order: `eraseInstruction` asserts that nothing reads what it removes.
    bool replace(Block& block, Size index, ModulePtr<Inst> pointer, Inst* replacement) {
        InstList instructions;
        instructions.push(replacement);

        return replaceWith(block, index, pointer, instructions, valueOf(replacement));
    }

    // The same, for a rule whose answer is more than one instruction: what goes in at `index`, and
    // which of it the readers of the old instruction are pointed at.
    bool replaceWith(Block& block, Size index, ModulePtr<Inst> pointer, InstList& instructions,
                     ModulePtr<Value> answer)
    {
        opt.ir().insert(block, index, instructions);
        opt.ir().replaceValue((ModulePtr<Value>)pointer, answer);
        opt.ir().eraseInstruction(pointer);
        return true;
    }

    void run() {
        bool_ = opt.program.scalar.bool_;
        if(!bool_) return;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(Size i = 0; i < block->instructionCount(); i++) {
                auto pointer = block->instructionAt(opt.local, i);
                auto kind = opt.local[pointer]->kind;

                auto before = block->instructionCount();
                auto rewrote = kind == Value::Select ? rewriteSelect(*block, i, pointer)
                             : rewriteDeMorgan(*block, i, pointer);
                if(!rewrote) continue;

                // The replacements went in at `i` and the old instruction came out, so the next one
                // this has not seen is wherever the list ends up: unmoved for a rule that answered
                // with one instruction, and one further along for De Morgan, which answers with two.
                i += block->instructionCount() - before;
            }
        }
    }
};

}

void reduceBooleanOperations(OptContext& opt) {
    BooleanReducer reducer { opt };
    reducer.run();
}
