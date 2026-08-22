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
 * `foldSelect` in opt_fold.cpp answers a select with a value that is already there - an arm, or a
 * constant. Three of the four rules below need an instruction that does not exist yet, and that pass
 * deliberately inserts none: its walk is an index over a list nothing is added to, which is the
 * property that lets it run first in the round. So this is its own walk, over the one instruction
 * kind it rewrites.
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
    bool rewrite(Block& block, Size index, ModulePtr<Inst> pointer) {
        auto& select = (InstSelect&)*opt.local[pointer];
        if(select.type != bool_) return false;
        if(opt.local[select.cond]->type != bool_) return false;

        auto whenTrue = constantValueOf(opt, select.whenTrue);
        auto whenFalse = constantValueOf(opt, select.whenFalse);

        // Two constants that are not the same one, which is `c` or its complement. The equal pair is
        // `foldSelect`'s and never reaches here.
        if(whenTrue && whenFalse) {
            if(whenTrue.unwrap() == 1 && whenFalse.unwrap() == 0) {
                opt.ir().replaceValue((ModulePtr<Value>)pointer, select.cond);
                opt.ir().eraseInstruction(pointer);
                return true;
            }

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

    // The replacement put where the select was, its readers pointed at it, and the select erased.
    // In that order: `eraseInstruction` asserts that nothing reads what it removes.
    bool replace(Block& block, Size index, ModulePtr<Inst> pointer, Inst* replacement) {
        InstList instructions;
        instructions.push(replacement);

        opt.ir().insert(block, index, instructions);
        opt.ir().replaceValue((ModulePtr<Value>)pointer, valueOf(replacement));
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
                if(opt.local[pointer]->kind != Value::Select) continue;

                auto before = block->instructionCount();
                if(!rewrite(*block, i, pointer)) continue;

                // The replacement went in at `i` and the select came out, so the next instruction
                // this has not seen is wherever the list ends up - one shorter where the answer was
                // a value that already existed, and the same length where it was an instruction.
                i += block->instructionCount() - before;
            }
        }
    }
};

}

void reduceBooleanSelects(OptContext& opt) {
    BooleanReducer reducer { opt };
    reducer.run();
}
