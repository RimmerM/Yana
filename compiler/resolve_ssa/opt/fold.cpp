#include "pass.h"
#include "../../lower/fold_arith.h"
#include "../../resolve_mir/fold.h"

/*
 * The arithmetic: what an instruction over constants comes to, and the identities that need no
 * constant at all.
 *
 * `simplify.cpp` is the walk; this is the table it consults, and everything here answers with a
 * *value* - a constant this made, or an operand that was already standing there. Nothing is rewritten
 * in place and nothing is erased, which is what lets one worklist drive five rewrites.
 *
 * ## The two rules
 *
 * **A folded result is stored the way the register would have held it.** A narrow value is in normal
 * form at this tier - sign-extended where the type is signed, masked where it is not - and
 * `resolve_ssa/lower/width.cpp` is where that is established for a value the folder cannot see
 * through. `narrowToWidth` is the same rule applied to a constant, and it is the same one
 * `mir::reducedToWidth` already applies to a literal, so a number this folder writes and a number the
 * resolver wrote are the same bits.
 *
 * **Where the answer is the target's rather than the language's, it is not folded.** Three cases, and
 * each is named where it is declined: a **target-answered width**, whose bits are the representation
 * layer's to decide; a **shift by a distance at or above the width**, which every machine masks its
 * own way; and a **float-to-integer conversion of a value the integer cannot hold**, which is a
 * ruling this stage has not made. `Metric` is a fourth of the same shape and is declined for the
 * same reason as the first.
 *
 * ## `@bits(n)` folds here, and does not in the shipping stage
 *
 * That difference is the decision this file makes, and it is not a change of nerve. The shipping
 * stage declines a refinement because its two backends disagree about what a refinement's
 * *arithmetic* wraps at - `narrowerThanRegister` in `resolve/lower.cpp` excludes refinements, so
 * native computes at the unrefined width, while `coerce` in `codegen/js/type.cpp` masks every
 * integer to its own `bits`. Folding one there would have been choosing a side.
 *
 * There is no such disagreement here. `resolve_ssa/lower/width.cpp` says so in as many words - "a
 * `@bits(n)` refinement is **not** a second rule here… it is an integer type whose declared width is
 * narrower than its register, which is exactly what this file is written over" - so a `convert` whose
 * result is a `@bits(20) U32` is put back into twenty bits by the same mask a `U8` addition gets, and
 * `op.def`'s width column is what decides which operations owe one. So the width a refinement wraps
 * at is stated, it is the refinement's own `n`, and folding at it reproduces what the lowering does
 * rather than approximating it. `FoldBits.yana` is the fixture, and its `.run.expect` is shared with
 * the shipping stage, which is what says the two agree about the answer even where they disagree
 * about who computes it.
 *
 * ## Where the arithmetic itself lives
 *
 * `compiler/lower/fold_arith.h`, shared with `lower/lower_fold.cpp` - see the header, which is the
 * argument for the sharing and for the one thing that had to be mapped: the lower IR's kinds carry
 * the signedness that `mir::BinaryOp` reads off the operand type, so `arithKind` below is the table
 * that says which row a signed `div` is. What is *not* shared is the normal form: that evaluator
 * masks its answer to the width, because the lower tier has nothing narrower than a register to
 * extend to, and `narrowToWidth` here puts the same number into this tier's form.
 */

namespace ssa {

using foldArith::evaluateBinary;
using foldArith::maskOf;

Maybe<IntFacts> foldableInt(OptContext& opt, DeclPtr<mir::Type> type) {
    if(!type) return Nothing();

    /*
     * `Bool` is one unsigned bit, and it is a declared enum in this stage rather than a type kind -
     * `data Bool = False | True` in `lib/Core/Core.yana`, claiming `known.def`'s role. Its
     * constants are constructor indices, which are 0 and 1 by the rule `checkRoles` holds that
     * declaration to, so the three connectives `Bitwise(Bool)` declares are one-bit arithmetic over
     * exactly the numbers this says they are.
     */
    if(type == mir::booleanType(opt.program)) return Just(IntFacts { 1, false });

    if(opt.decl[type]->kind != mir::Type::Int) return Nothing();
    auto& integer = *(mir::IntType*)opt.decl[type];

    // The representation layer's question, declined - see the note on the declaration in pass.h, and
    // `mir::reducedToWidth`, which leaves a target-answered width alone for the same reason.
    if(integer.isTargetWidth()) return Nothing();

    // A `@bits(n)` refinement at its own `n`, which is what `bits` already holds - see the file
    // comment for why this stage may and the shipping stage may not.
    if(integer.bits == 0 || integer.bits > 64) return Nothing();

    return Just(IntFacts { integer.bits, integer.isSigned });
}

Maybe<IntWidths> foldableIntWidths(OptContext& opt, DeclPtr<mir::Type> type) {
    // Everything that states its own width answers it twice, which is what makes a caller of this
    // one walk rather than two - see the declaration.
    if(auto fixed = foldableInt(opt, type)) return Just(IntWidths { fixed.unwrap(), fixed.unwrap().bits });

    if(!type || opt.decl[type]->kind != mir::Type::Int) return Nothing();
    auto& integer = *(mir::IntType*)opt.decl[type];
    if(!integer.isTargetWidth()) return Nothing();

    auto narrow = U8(integer.minBits());
    auto wide = U8(integer.maxBits());
    if(narrow == 0 || wide > 64 || narrow > wide) return Nothing();

    return Just(IntWidths { IntFacts { narrow, integer.isSigned }, wide });
}

Maybe<U8> foldableFloat(OptContext& opt, DeclPtr<mir::Type> type) {
    if(!type || opt.decl[type]->kind != mir::Type::Float) return Nothing();
    return Just(((mir::FloatType*)opt.decl[type])->bits);
}

U64 narrowToWidth(U64 value, const IntFacts& facts) {
    if(facts.bits >= 64) return value;

    auto mask = maskOf(facts.bits);
    auto masked = value & mask;

    // Sign-extended rather than masked, because that is the register a signed narrow value is held
    // in - `putInRange` in `lower/width.cpp` shifts up and arithmetically back down - and because
    // this is the form `mir::reducedToWidth` leaves a literal in. One stored form satisfies both.
    if(facts.isSigned && (masked & (U64(1) << (facts.bits - 1)))) return masked | ~mask;

    return masked;
}

// The constant an instruction is, or null. `Value::definer` is what makes this one lookup rather
// than a search: a value is defined by exactly one instruction or is a block argument, and a block
// argument is never a constant.
const mir::Constant* constantOf(OptContext& opt, Value* value) {
    if(!value || !value->definer) return nullptr;

    auto definer = opt.base[value->definer];
    if(definer->kind != Inst::Const) return nullptr;

    return &((InstConst&)*definer).constant;
}

namespace {

// Whether one operand is exactly this number at its own type.
bool isConstantValue(OptContext& opt, Use used, U64 wanted) {
    auto found = constantValueOf(opt, valueOf(opt, used));
    return found && found.unwrap() == wanted;
}

bool isFloatValue(OptContext& opt, Use used, F64 wanted) {
    auto found = constantFloatOf(opt, valueOf(opt, used));
    return found && found.unwrap() == wanted;
}

/*
 * The signedness two operands are ordered at, from the **operand** type - which is the question the
 * result type says nothing about, since it is `Bool`.
 *
 * `foldableIntWidths` rather than `factsOf`, because what a comparison needs from the type is its
 * signedness and not a width to compute at: each constant already holds at the guaranteed width,
 * `constantValueOf` hands both back extended to sixty-four bits by that type's own sign rule, and
 * comparing those two patterns is comparing the two numbers. So this is one of the folds a
 * target-answered width needs no trial run for - and asking `factsOf` outside one would have
 * answered nothing about the sign and read a negative `Size` as a large positive one.
 *
 * A type with no width at all answers unsigned, which is right for the one kind that reaches here
 * without one. A payload-free enum has no width and needs none: `constantValueOf` reads such a
 * constant as its **constructor index**, which is a small non-negative number, and
 * `OpCategory::Equality` is the only category that admits one - an enum orders through `Ordering`
 * and a `compare` call, never through `lt` at the enum itself. So the two indices are compared
 * unsigned and `@value(n)` cannot move the answer, since an index and the number stored for it are
 * a bijection. That is what makes `compare(a, b) == LT` fold, which is where every default `Ord`
 * writes ends up once its instance has been selected.
 */
bool compareSignedness(OptContext& opt, DeclPtr<mir::Type> type) {
    auto facts = foldableIntWidths(opt, type);
    return facts && facts.unwrap().narrow.isSigned;
}

/*
 * One arithmetic run at one width, over two numbers a caller is holding.
 *
 * The shared evaluator with this tier's two conventions either side of it: the operands are masked on
 * the way in because a signed narrow value arrives sign-extended and `evaluateBinary` reads its
 * operands as the low `bits` of their patterns - `fold_arith.h` says so - and the answer is put into
 * this tier's normal form on the way out.
 *
 * Two readers, and the same three lines either way: `Folder::foldBinary` writes the answer into the
 * IR, and `binaryOfConstants` hands it back to a caller that has no constant in the IR to fold.
 */
Maybe<U64> evaluateAtWidth(BinaryOp op, const IntFacts& facts, U64 left, U64 right) {
    LowerInst::Kind kind;
    U64 result;

    if(!mir::arithKind(op, facts.isSigned, kind)) return Nothing();
    if(!evaluateBinary(kind, left & maskOf(facts.bits), right & maskOf(facts.bits), facts.bits,
                       result)) {
        return Nothing();
    }

    return Just(narrowToWidth(result, facts));
}

/*
 * Whether the two operands are the same SSA value, which is what makes `x - x` and `x ^ x` zero
 * without either being a constant.
 *
 * Value identity is enough and purity is not required: this is SSA, so one value is one evaluation
 * however expensive it was. `sub %v, %v` where `%v` is a call result is still zero, and the call
 * still happens - what goes away is the subtraction, not its operand.
 */
bool sameOperand(Use a, Use b) {
    return a.isSet() && a.value() == b.value();
}

struct Folder {
    OptContext& opt;
    Inst& inst;

    /*
     * The width every **target-answered** type is read at inside the run in progress, or zero
     * outside one - see `foldAtWidths`, which is the only thing that sets it.
     *
     * A field rather than an argument because the width is asked for in more places than the rule
     * being run is handed: a bit count and a byte reversal ask it of their *operand's* type, and a
     * conversion asks it of the source. One of those reading `minBits` while the run computed at
     * `maxBits` is a number that belongs to neither target, so all of them go through `factsOf`.
     */
    U8 targetBits = 0;

    // Whether this is one of the two trial runs, which write nothing into the IR: `constant` records
    // the number it was going to make and answers with a marker instead.
    bool trial = false;
    Maybe<U64> trialNumber;

    Value* constant(DeclPtr<mir::Type> type, U64 value) {
        if(trial) {
            trialNumber = Just(value);
            return opt.base[inst.results.get(opt.base, 0)];
        }

        return makeConstant(opt, inst, type, value);
    }

    /*
     * The width to compute a type at, inside whichever run is in progress.
     *
     * `foldableInt` for everything that states its own width, and the run's own width for a
     * target-answered one. Every rule in this file asks through here rather than through
     * `foldableInt` directly, which is what makes the two runs below runs of the *same* fold.
     */
    Maybe<IntFacts> factsOf(DeclPtr<mir::Type> type) {
        if(auto fixed = foldableInt(opt, type)) return fixed;
        if(!targetBits) return Nothing();

        auto widths = foldableIntWidths(opt, type);
        if(!widths) return Nothing();

        return Just(IntFacts { targetBits, widths.unwrap().narrow.isSigned });
    }

    /*
     * One rule, run at each width its result type may have, and answered only where the two agree.
     *
     * A type that states its own width runs once, and every fold in this file that ever fired still
     * fires exactly as it did. A **target-answered** one runs twice with nothing written down, and
     * the two answers have to be the same one: the same existing value, or the same number. See
     * `IntWidths` in `pass.h`, where what "the same" buys and what it costs are set out.
     *
     * The trial runs answer a *marker* where they would have made a constant - the instruction's own
     * result, which is a value no rule can otherwise return - so that "both made a number" and "both
     * answered the same operand" are one pointer comparison plus the numbers beside it.
     */
    template<class Rule> Value* foldAtWidths(Rule&& rule, DeclPtr<mir::Type> type) {
        auto widths = foldableIntWidths(opt, type);
        if(!widths) return nullptr;

        auto narrow = widths.unwrap().narrow;
        if(!widths.unwrap().differ()) return rule(narrow);

        auto wide = IntFacts { widths.unwrap().wide, narrow.isSigned };

        trial = true;

        targetBits = narrow.bits;
        trialNumber = Nothing();
        auto first = rule(narrow);
        auto firstNumber = trialNumber;

        targetBits = wide.bits;
        trialNumber = Nothing();
        auto second = rule(wide);
        auto secondNumber = trialNumber;

        trial = false;
        targetBits = 0;

        if(!first || first != second) return nullptr;
        if(bool(firstNumber) != bool(secondNumber)) return nullptr;
        if(firstNumber && firstNumber.unwrap() != secondNumber.unwrap()) return nullptr;

        targetBits = narrow.bits;
        auto folded = rule(narrow);
        targetBits = 0;

        return folded;
    }

    DeclPtr<mir::Type> resultType() { return opt.base[inst.results.get(opt.base, 0)]->type(); }

    Value* operand(Use used) { return valueOf(opt, used); }
    DeclPtr<mir::Type> typeOf(Use used) { return operand(used)->type(); }

    /*
     * The six orderings, over patterns and over doubles - `mir::compareIntegers` and
     * `mir::compareFloats`, which the constant tier folds a written comparison with.
     *
     * The operand type decides the integer one rather than the result type, which is `Bool` and says
     * nothing about how the two sides are ordered. A signed narrow value arrives here sign-extended
     * to 64 bits by `narrowToWidth`, so comparing as `I64` is comparing at the type's own width.
     * IEEE 754 specifies the float six exactly, so there is nothing about width or sign in that one
     * and nothing about NaN either, because a constant that is one is declined before it arrives.
     */
    static bool compare(BinaryOp op, U64 lhs, U64 rhs, bool isSigned) {
        return mir::compareIntegers(op, lhs, rhs, isSigned);
    }

    static bool compareFloat(BinaryOp op, F64 lhs, F64 rhs) {
        return mir::compareFloats(op, lhs, rhs);
    }

    // Which way one of the six answers about a value and itself.
    static bool compareReflexive(BinaryOp op) {
        return op == BinaryOp::Eq || op == BinaryOp::Ge || op == BinaryOp::Le;
    }

    /*
     * A select of two constants, compared against a third - the shape a widened boolean is tested in.
     *
     * Once a `Bool` has been widened and put back through a comparison, what is left of it is
     * `select c, 1, 0` and what is left of the test is `eq` of that against `1`. The comparison has
     * one answer per arm and both are decidable here, so the only question is which of the four
     * pairs it came out as.
     *
     * Three of the four are answerable with a value that already exists: the two arms agreeing is a
     * constant, and agreeing *with the condition* is the condition. The fourth is `!c`, which is an
     * instruction rather than a value - and it is declined here on the same terms the mirror pair in
     * `simplify.cpp` is: whether a complement is free is a question about the rest of the function,
     * and what it costs where it is not is one `xor` that was going to be emitted anyway.
     *
     * The select is looked for on **both** sides, which is this file's convention, and the operand
     * order is carried into the comparison rather than assumed away - four of the six orderings are
     * not symmetric.
     */
    Value* foldCompareOfSelect(InstBinary& binary, bool isSigned) {
        auto onLeft = operand(binary.lhs)->definer &&
                      opt.base[operand(binary.lhs)->definer]->kind == Inst::Select;
        auto held = operand(onLeft ? binary.lhs : binary.rhs);
        if(!held->definer) return nullptr;

        auto definer = opt.base[held->definer];
        if(definer->kind != Inst::Select) return nullptr;

        auto& select = (InstSelect&)*definer;
        auto whenTrue = constantValueOf(opt, operand(select.whenTrue));
        auto whenFalse = constantValueOf(opt, operand(select.whenFalse));
        auto against = constantValueOf(opt, operand(onLeft ? binary.rhs : binary.lhs));
        if(!whenTrue || !whenFalse || !against) return nullptr;

        auto answer = [&](U64 arm) {
            return onLeft ? compare(binary.op, arm, against.unwrap(), isSigned)
                          : compare(binary.op, against.unwrap(), arm, isSigned);
        };

        auto ifTrue = answer(whenTrue.unwrap());
        if(ifTrue == answer(whenFalse.unwrap())) return constant(resultType(), ifTrue ? 1 : 0);

        // The condition itself, at the comparison's own type - which is `Bool` for both, but asked
        // rather than assumed, since what is handed back is one value in place of another.
        if(ifTrue && typeOf(select.condition) == resultType()) return operand(select.condition);

        return nullptr;
    }

    /*
     * Whether this value is the address of one of this body's own frame allocations, through the
     * bitcasts a library boundary spells an address with.
     *
     * Frame and not every class, which is the whole of the caution: an allocation that asked an
     * allocator is one the allocator may have refused, and a body that tests for it is asking a real
     * question. A frame slot has an address by construction.
     */
    bool isFrameAddress(Value* value) {
        while(value && value->definer) {
            auto definer = opt.base[value->definer];
            if(definer->kind == Inst::Alloc) {
                return ((InstAlloc*)definer)->storage == mir::StorageClass::Frame;
            }

            if(definer->kind != Inst::Unary) return false;
            if(((InstUnary*)definer)->op != mir::UnaryOp::Bitcast) return false;

            value = opt.base[((InstUnary*)definer)->value.value()];
        }

        return false;
    }

    // Zero at whatever width, read off the instruction because a null pointer's is the target's -
    // see `countOf` in `opt/place.cpp`, where the same reading is argued.
    bool isNullConstant(Value* value) {
        auto held = constantOf(opt, value);
        return held && held->kind == mir::Constant::Int && held->asInt() == 0;
    }

    /*
     * A comparison, whose result type is `Bool` and whose every question is about the operands.
     *
     * The reflexive answer is given ahead of any type gate, because an ordering is reflexive whatever
     * the operands are made of and needs neither a width nor a signedness. Two exclusions, and they
     * are the two the shipping stage's folder names:
     *
     *  - a **float**, for the one reason IEEE 754 gives - a NaN is not equal to itself, so none of
     *    the six is reflexive over the type as a whole;
     *  - a **vector**, whose answer is right and whose *shape* is not. `eq %v, %v` over an integer
     *    vector really is true of every lane, and what says so is an all-ones mask rather than the
     *    number one. Nothing produces a vector comparison yet, so this is a guard rather than a fix.
     */
    Value* foldCompare(InstBinary& binary) {
        auto operandType = typeOf(binary.lhs);
        auto isFloat = foldableFloat(opt, operandType);

        if(mir::isVectorType(opt.decl, operandType)) return nullptr;

        /*
         * A frame allocation's address against null - the refusal test a library writes in front of
         * storage it did not place itself, and the one comparison here whose operands are not both
         * constants and whose answer is still known.
         *
         * `runOver` is what asks it: it takes bytes from whoever placed them and answers the empty
         * run where there are none, so a run the compiler put in the frame arrives at a test with
         * one possible answer. Without this the arm that cannot be taken stays, and with it the
         * whole refusal - a branch, a second `Run` and a join - folds away.
         */
        if(mir::binaryCategory(binary.op) == mir::OpCategory::Equality) {
            auto left = operand(binary.lhs);
            auto right = operand(binary.rhs);

            if((isFrameAddress(left) && isNullConstant(right)) ||
               (isFrameAddress(right) && isNullConstant(left))) {
                return constant(resultType(), binary.op == BinaryOp::Eq ? 0 : 1);
            }
        }

        if(!isFloat && sameOperand(binary.lhs, binary.rhs)) {
            return constant(resultType(), compareReflexive(binary.op) ? 1 : 0);
        }

        if(isFloat) {
            auto left = constantFloatOf(opt, operand(binary.lhs));
            auto right = constantFloatOf(opt, operand(binary.rhs));
            if(!left || !right) return nullptr;
            if(!isFoldableFloat(left.unwrap()) || !isFoldableFloat(right.unwrap())) return nullptr;

            return constant(resultType(), compareFloat(binary.op, left.unwrap(), right.unwrap()) ? 1 : 0);
        }

        // The signedness the two patterns are ordered at - `compareSignedness`, which is where the
        // reading is argued and which `binaryOfConstants` asks the same question through.
        auto isSigned = compareSignedness(opt, operandType);

        // The select rule ahead of the both-constants read, because its operand is a select rather
        // than a constant and the read below would end the walk before it was reached.
        if(auto folded = foldCompareOfSelect(binary, isSigned)) return folded;

        auto left = constantValueOf(opt, operand(binary.lhs));
        auto right = constantValueOf(opt, operand(binary.rhs));
        if(!left || !right) return nullptr;

        return constant(resultType(), compare(binary.op, left.unwrap(), right.unwrap(), isSigned) ? 1 : 0);
    }

    /*
     * Floating arithmetic over two constants, and the two identities floating arithmetic has.
     *
     * IEEE 754 specifies add, subtract, multiply and divide exactly and the target uses the
     * hardware's, so `F64` is the host's own `double` and there is nothing to say. `F32` is rounded
     * back to single after each operation, which is what makes a *chain* of them the same value the
     * machine computes rather than a double result rounded once at the end.
     *
     * **`x * 1` and `x / 1` are the whole list**, and the ones an integer reader would expect next
     * are all wrong. `x + 0` is not `x`, because `-0 + 0` is `+0`. `x * 0` is not `0`, because
     * `NaN * 0` is `NaN` and `-1 * 0` is `-0`. `x - x` is not `0`, because an infinity minus itself
     * is a NaN. Each of those is an identity over the reals and none of them is one over IEEE 754,
     * which is what the type actually is.
     *
     * A non-finite result is declined rather than folded: dividing by zero is the way to reach one,
     * and there is no literal for an infinity or a NaN to write it as. `rem` is left alone entirely,
     * since what it means for floats is not a question this needs an opinion about.
     */
    Value* foldFloatBinary(InstBinary& binary) {
        auto left = constantFloatOf(opt, operand(binary.lhs));
        auto right = constantFloatOf(opt, operand(binary.rhs));

        if(!left || !right) {
            if(binary.op == BinaryOp::Mul || binary.op == BinaryOp::Div) {
                if(isFloatValue(opt, binary.rhs, 1.0)) return operand(binary.lhs);
            }

            if(binary.op == BinaryOp::Mul && isFloatValue(opt, binary.lhs, 1.0)) {
                return operand(binary.rhs);
            }

            return nullptr;
        }

        if(!isFoldableFloat(left.unwrap()) || !isFoldableFloat(right.unwrap())) return nullptr;

        auto lhs = left.unwrap(), rhs = right.unwrap();
        F64 result;

        switch(binary.op) {
            case BinaryOp::Add: result = lhs + rhs; break;
            case BinaryOp::Sub: result = lhs - rhs; break;
            case BinaryOp::Mul: result = lhs * rhs; break;
            case BinaryOp::Div:
                if(rhs == 0.0) return nullptr;
                result = lhs / rhs;
                break;
            default:
                return nullptr;
        }

        if(!isFoldableFloat(result)) return nullptr;
        return makeFloatConstant(opt, inst, resultType(), result);
    }

    /*
     * The integer arithmetic and the identities, at the result type's own width.
     *
     * Each identity is written against **both** operands where the operation is commutative, which is
     * the one place this differs from the shipping stage's folder: that one canonicalizes a constant
     * onto the right first and then writes each rule once. Canonicalizing is a rewrite in place, and
     * everything here answers with a value - see the file comment - so the rules are written out
     * twice instead. The set is small enough that the trade is not close.
     */
    Value* foldBinary(InstBinary& binary, const IntFacts& facts) {
        auto type = resultType();
        auto left = constantValueOf(opt, operand(binary.lhs));
        auto right = constantValueOf(opt, operand(binary.rhs));

        auto wrap = [&](U64 value) { return constant(type, narrowToWidth(value, facts)); };
        auto zero = [&] { return wrap(0); };
        auto all = narrowToWidth(~U64(0), facts);

        // Both known: the shared evaluator at the declared width - `evaluateAtWidth`, which is where
        // the two conventions around it are written and which `binaryOfConstants` runs through too.
        if(left && right) {
            if(auto folded = evaluateAtWidth(binary.op, facts, left.unwrap(), right.unwrap())) {
                return constant(type, folded.unwrap());
            }
        }

        switch(binary.op) {
            case BinaryOp::Add:
                if(isConstantValue(opt, binary.rhs, 0)) return operand(binary.lhs);
                if(isConstantValue(opt, binary.lhs, 0)) return operand(binary.rhs);
                break;
            case BinaryOp::Sub:
                if(isConstantValue(opt, binary.rhs, 0)) return operand(binary.lhs);
                if(sameOperand(binary.lhs, binary.rhs)) return zero();
                break;
            case BinaryOp::Mul:
                if(isConstantValue(opt, binary.rhs, 1)) return operand(binary.lhs);
                if(isConstantValue(opt, binary.lhs, 1)) return operand(binary.rhs);
                if(isConstantValue(opt, binary.rhs, 0) || isConstantValue(opt, binary.lhs, 0)) {
                    return zero();
                }
                break;

            /*
             * The one identity `mulhi` has, and it is not `mul`'s.
             *
             * Multiplying by one is not one of them: the top half of `x * 1` is zero for an unsigned
             * type and `x`'s sign for a signed one, not `x`. Multiplying by zero is, and it is the
             * same zero at both signednesses.
             */
            case BinaryOp::MulHi:
                if(isConstantValue(opt, binary.rhs, 0) || isConstantValue(opt, binary.lhs, 0)) {
                    return zero();
                }
                break;

            // `x / 0` is 0 and `x % 0` is `x`, which the *language* says rather than a target - see
            // the ruling in `doc/spec/types.md` that `makeDivisionTotal` emits. So a zero divisor
            // needs nothing known about the dividend.
            case BinaryOp::Div:
                if(isConstantValue(opt, binary.rhs, 1)) return operand(binary.lhs);
                if(isConstantValue(opt, binary.rhs, 0)) return zero();
                if(isConstantValue(opt, binary.lhs, 0)) return zero();
                break;
            case BinaryOp::Rem:
                if(isConstantValue(opt, binary.rhs, 0)) return operand(binary.lhs);
                if(isConstantValue(opt, binary.rhs, 1)) return zero();
                if(isConstantValue(opt, binary.lhs, 0)) return zero();
                break;

            /*
             * A shift by zero, and a shift *of* zero - the second being the one that has an answer at
             * every distance, including the ones the evaluator above declines. Nothing a machine does
             * to an out-of-range count can turn zero into anything else.
             */
            case BinaryOp::Shl:
            case BinaryOp::Shr:
            case BinaryOp::Sar:
                if(isConstantValue(opt, binary.rhs, 0)) return operand(binary.lhs);
                if(isConstantValue(opt, binary.lhs, 0)) return zero();
                break;

            /*
             * The same two for a rotation, and the zero-count one is tested **modulo the width**: a
             * rotation by the width is the identity where a shift by it is not, which is the whole
             * difference between the two kinds.
             *
             * The count is masked to the width *before* the modulus, which is not a spare `and`: the
             * evaluator above reads a count as the low `bits` of its pattern, and this tier holds a
             * signed narrow constant sign-extended - so `-1` at a `@bits(5) I32` is `~0` here and a
             * count of 31 there. At a width that divides `2^64 - 1` the two then disagree about
             * whether the rotation is the identity: `(~U64(0)) % 5` is zero and `(31 & 31) % 5` is
             * one. Asking the same masked question is what keeps the identity and the evaluation
             * from being two answers to one operation.
             */
            case BinaryOp::Rol:
            case BinaryOp::Ror:
                if(right && ((right.unwrap() & maskOf(facts.bits)) % facts.bits) == 0) {
                    return operand(binary.lhs);
                }
                if(isConstantValue(opt, binary.lhs, 0)) return zero();
                break;

            case BinaryOp::And:
                if(isConstantValue(opt, binary.rhs, 0) || isConstantValue(opt, binary.lhs, 0)) {
                    return zero();
                }
                if(isConstantValue(opt, binary.rhs, all)) return operand(binary.lhs);
                if(isConstantValue(opt, binary.lhs, all)) return operand(binary.rhs);
                if(sameOperand(binary.lhs, binary.rhs)) return operand(binary.lhs);
                break;
            case BinaryOp::Or:
                if(isConstantValue(opt, binary.rhs, 0)) return operand(binary.lhs);
                if(isConstantValue(opt, binary.lhs, 0)) return operand(binary.rhs);

                // The dual of the `and` above, and the one that pays at a width other than a word:
                // `~0` at a `Bool` is `True`, so `x || True` arriving as `or x, True` is the constant
                // it always was. The `Bool` rewrite in simplify.cpp is what makes that a shape.
                if(isConstantValue(opt, binary.rhs, all) || isConstantValue(opt, binary.lhs, all)) {
                    return wrap(~U64(0));
                }
                if(sameOperand(binary.lhs, binary.rhs)) return operand(binary.lhs);
                break;
            case BinaryOp::Xor:
                if(isConstantValue(opt, binary.rhs, 0)) return operand(binary.lhs);
                if(isConstantValue(opt, binary.lhs, 0)) return operand(binary.rhs);
                if(sameOperand(binary.lhs, binary.rhs)) return zero();
                break;

            /*
             * The low bits kept. A count at or above the width keeps everything and a count of zero
             * keeps nothing, which is the library's ruling rather than the instruction's - `bzhi`
             * reads its count from a byte, so 256 is a count of zero to it.
             *
             * The half that is a *rewrite* rather than an answer - a count below the width, which is
             * an `and` against a literal - is deliberately not here. It inserts an instruction, and
             * `optimizeLowerFunction` performs it one tier down, where the same constant count
             * arrives.
             */
            case BinaryOp::BitsUpTo:
                if(right && right.unwrap() >= facts.bits) return operand(binary.lhs);
                if(isConstantValue(opt, binary.rhs, 0)) return zero();
                if(isConstantValue(opt, binary.lhs, 0)) return zero();
                break;

            default:
                break;
        }

        return nullptr;
    }

    /*
     * `(x op c1) op c2` becomes `x op (c1 op c2)`, for the operations where that is an identity.
     *
     * The five bitwise-and-arithmetic ones are associative **as modular arithmetic**, which is what
     * the type says they are: both instructions have this result type, so both wrap at the same
     * width and the combined constant wraps with them. The three shifts are not associative and are
     * here for the identity they do have - two shifts in one direction are one shift by the sum -
     * which needs the sum to be a distance the language has an answer for, so all three counts are
     * required to be **below the width**. At or above it is the second case of this file's rule two:
     * every machine masks its own way and nothing here decides which.
     *
     * A rotation is left out, and not because the identity is false: `(x rol c1) rol c2` is
     * `x rol ((c1 + c2) mod width)` at every pair of counts. It is left out because nothing writes
     * one twice - measured at zero over both corpora, where the shift chain is a real shape - and a
     * rule that fires on nothing is a rule that is not there to be got wrong.
     *
     * **The inner operation may have other readers**, and then this is neutral rather than a shrink:
     * one instruction and one constant replace one instruction and one constant, and the inner stays
     * standing for its other reader. It is admitted anyway because the *chain* is what pays - the
     * value this answers with is one a further reassociation above it can take - and because two
     * `x op c` at one constant are then one computation for `cse`.
     *
     * This is the one rule in the file that answers with the result of an instruction it **made**
     * rather than with a constant or an operand already standing there, which `simplify`'s
     * `replaceAndErase` handles identically: the folder still rewrites nothing in place and erases
     * nothing, which is the property the worklist rests on.
     */
    Value* reassociate(InstBinary& binary, const IntFacts& facts) {
        auto op = binary.op;

        auto isShift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::Sar;
        auto isCombining = op == BinaryOp::Add || op == BinaryOp::Mul || op == BinaryOp::And ||
                           op == BinaryOp::Or || op == BinaryOp::Xor;
        if(!isShift && !isCombining) return nullptr;

        /*
         * Which side of this instruction the constant is on, and which is the inner operation. A
         * shift takes its count on the right and nowhere else; the other five are commutative and
         * both orders are written out, which is this file's convention.
         *
         * Both constants are required to be at the **result's** own type, which for the five
         * `Closed` operations they are by construction and for a shift's count is a thing to ask:
         * the combined number is written at that type, so a count the author gave another one would
         * come back as an operand of a type the original never had.
         */
        auto atResult = [&](Use used) { return typeOf(used) == resultType(); };

        auto outer = atResult(binary.rhs) ? constantValueOf(opt, operand(binary.rhs)) : Nothing();
        auto inner = operand(binary.lhs);

        if(!outer && isCombining) {
            outer = atResult(binary.lhs) ? constantValueOf(opt, operand(binary.lhs)) : Nothing();
            inner = operand(binary.rhs);
        }

        if(!outer || !inner->definer) return nullptr;

        auto definer = opt.base[inner->definer];
        if(definer->kind != Inst::Binary || ((InstBinary&)*definer).op != op) return nullptr;
        if(inner->type() != resultType()) return nullptr;

        auto& below = (InstBinary&)*definer;
        auto middle = atResult(below.rhs) ? constantValueOf(opt, valueOf(opt, below.rhs)) : Nothing();
        auto rest = valueOf(opt, below.lhs);

        if(!middle && isCombining) {
            middle = atResult(below.lhs) ? constantValueOf(opt, valueOf(opt, below.lhs)) : Nothing();
            rest = valueOf(opt, below.rhs);
        }

        if(!middle || !rest) return nullptr;

        /*
         * What combining the two constants *is*, named per operation rather than defaulted: a sixth
         * kind added to either list above declines here instead of being read as an `xor`.
         */
        U64 combined = 0;
        switch(op) {
            case BinaryOp::Add: combined = middle.unwrap() + outer.unwrap(); break;
            case BinaryOp::Mul: combined = middle.unwrap() * outer.unwrap(); break;
            case BinaryOp::And: combined = middle.unwrap() & outer.unwrap(); break;
            case BinaryOp::Or:  combined = middle.unwrap() | outer.unwrap(); break;
            case BinaryOp::Xor: combined = middle.unwrap() ^ outer.unwrap(); break;

            case BinaryOp::Shl:
            case BinaryOp::Shr:
            case BinaryOp::Sar: {
                // Read as counts rather than as patterns, which is the same masking the evaluator
                // does - a signed narrow constant is held sign-extended here, so `1` at a
                // `@bits(5) I32` and `~0` are two spellings the width has to separate.
                auto first = middle.unwrap() & maskOf(facts.bits);
                auto second = outer.unwrap() & maskOf(facts.bits);
                if(first >= facts.bits || second >= facts.bits) return nullptr;
                if(first + second >= facts.bits) return nullptr;

                combined = first + second;
                break;
            }

            default: return nullptr;
        }

        // The constant first and the cursor after it, in that order: `makeConstant` builds an editor
        // of its own, so a cursor taken before it is an index the insertion has moved and the binary
        // would land in front of the constant it names.
        auto count = constant(resultType(), narrowToWidth(combined, facts));

        auto editor = opt.ir();
        editor.setInsertBefore(inst);

        // The occurrence and the convention are the original's rather than re-derived, which is the
        // rule `opt/inline.cpp`'s clone states for the same reason: this computes what that
        // instruction computed, and a second derivation is a second chance to disagree.
        auto& result = *opt.base[inst.results.get(opt.base, 0)];
        auto made = editor.addBinary(inst.source, op, editor.use(*rest, UseKind::Inspect),
                                     editor.use(*count, UseKind::Inspect), result.occurrence,
                                     result.convention);

        return editor.resultOf(*made);
    }

    /*
     * The unary operations over one constant.
     *
     * The three bit counts are folded at the **declared** width, which is what makes them answer the
     * function the machine answers: the operand arrives sign-extended for a signed type, so the
     * register holds bits above the width that are the sign rather than the value, and counting them
     * would be counting the register's padding. `op.def` says the same from the other side, where
     * `hasMachineWidth` is what admits a type to these at all.
     *
     * The five float-only operations - `sqrt` and the four roundings - are declined. Each is exactly
     * specified and none of them is reachable without the host's own library, which nothing else in
     * this stage needs; what they cost is a constant that stays an instruction the machine performs
     * in one step anyway.
     */
    Value* foldUnary(InstUnary& unary, const IntFacts& facts) {
        auto from = constantValueOf(opt, operand(unary.value));
        if(!from) return nullptr;

        auto type = resultType();
        auto value = from.unwrap();

        switch(unary.op) {
            case UnaryOp::Neg:
                return constant(type, narrowToWidth(U64(0) - value, facts));

            /*
             * Every type, which is where this parts company with the shipping stage's folder.
             *
             * That one declines an *unsigned* type narrower than its register, because native leaves
             * `~(0 :: U8)` as `0xFFFFFFFF` while JS coerces it to 255 - a disagreement about what a
             * complement is. `not` is `Wraps` in `op.def` and `lower/width.cpp` puts the answer back
             * into the declared width unconditionally, so there is one answer here and this is it.
             */
            case UnaryOp::Not:
                return constant(type, narrowToWidth(~value, facts));

            // The magnitude, at the type's own width - and `Wraps` in `op.def` because the magnitude
            // of the minimum is the minimum again, which `narrowToWidth` is what produces.
            case UnaryOp::Abs:
                if(!facts.isSigned) return operand(unary.value);
                return constant(type, narrowToWidth(I64(value) < 0 ? U64(0) - value : value, facts));

            case UnaryOp::CountBits:
            case UnaryOp::LeadingZeros:
            case UnaryOp::TrailingZeros: {
                // The operand's own bits and not the register's - see above. `foldableInt` is asked of
                // the *operand* rather than of the result, which is where the width lives: all three
                // answer a small number that is in range of every type they are declared at.
                auto operandFacts = factsOf(typeOf(unary.value));
                if(!operandFacts) return nullptr;

                auto bits = U32(operandFacts.unwrap().bits);
                auto bitsValue = value & maskOf(bits);
                U64 answer = 0;

                if(unary.op == UnaryOp::CountBits) {
                    for(auto rest = bitsValue; rest; rest &= rest - 1) answer++;
                } else if(!bitsValue) {
                    // The language's answer at zero rather than the machine's - `op.def` states that
                    // both zero counts answer the type's own width, since `bsf`/`bsr` leave their
                    // destination unwritten and that is not a value this language can name.
                    answer = bits;
                } else if(unary.op == UnaryOp::TrailingZeros) {
                    while(!((bitsValue >> answer) & 1)) answer++;
                } else {
                    while(!((bitsValue >> (bits - 1 - answer)) & 1)) answer++;
                }

                return constant(type, narrowToWidth(answer, facts));
            }

            /*
             * The byte reversal, at the **declared** width for the bit counts' reason: the bytes an
             * operation exchanges are the type's, and a narrow value's register holds bits above
             * them that are the extension rather than the value.
             *
             * The double reversal is not here: it is an identity over an arbitrary operand rather
             * than an evaluation of a constant one, so it is decided in `run` before the gate that
             * requires the operand to be a constant at all.
             */
            case UnaryOp::ByteSwap: {
                auto operandFacts = factsOf(typeOf(unary.value));
                if(!operandFacts) return nullptr;

                auto bits = U32(operandFacts.unwrap().bits);
                auto pattern = value & maskOf(bits);
                U64 reversed = 0;

                for(U32 byte = 0; byte < bits / 8; byte++) {
                    reversed = (reversed << 8) | ((pattern >> (byte * 8)) & 0xFF);
                }

                return constant(type, narrowToWidth(reversed, facts));
            }

            default:
                return nullptr;
        }
    }

    // The width a `bitcast` moves, for either kind of scalar and 0 for everything else. Both ends of
    // every rung of the `Bitcast` ladder answer this with the same number, which is what makes the
    // reinterpretation below a reading of bits rather than a conversion of them.
    U8 reinterpretedBits(DeclPtr<mir::Type> type) {
        if(auto facts = factsOf(type)) return facts.unwrap().bits;
        if(auto width = foldableFloat(opt, type)) return width.unwrap();

        return 0;
    }

    /*
     * A `bitcast` of a constant: the same bits, read as another type.
     *
     * Not a conversion under another name. That one *converts*, and the two agree only where the
     * widths do - so the width is checked here rather than assumed, and a pointer, whose
     * `reinterpretedBits` is zero, is declined outright. `null()` is a bitcast at a pointer and its
     * operand is an integer constant this must not reinterpret as an address.
     *
     * The integer pair is `narrowToWidth` at the *result's* signedness, which is the whole of what
     * reinterpretation between two integers means: `0xffffffff` read as an `I32` is stored as `-1`,
     * and that is the form whatever folds next reads back.
     */
    Value* foldBitcast(InstUnary& unary) {
        auto sourceType = typeOf(unary.value);
        if(sourceType == resultType()) return operand(unary.value);

        auto bits = reinterpretedBits(sourceType);
        if(bits == 0 || bits != reinterpretedBits(resultType())) return nullptr;

        if(auto facts = foldableInt(opt, resultType())) {
            U64 raw;

            if(foldableFloat(opt, sourceType)) {
                auto source = constantFloatOf(opt, operand(unary.value));
                if(!source) return nullptr;

                auto single = F32(source.unwrap());
                if(bits == 32) {
                    U32 word;
                    copy((const Byte*)&single, (Byte*)&word, 4);
                    raw = word;
                } else {
                    auto wide = source.unwrap();
                    copy((const Byte*)&wide, (Byte*)&raw, 8);
                }
            } else {
                auto source = constantValueOf(opt, operand(unary.value));
                if(!source) return nullptr;

                raw = source.unwrap();
            }

            return constant(resultType(), narrowToWidth(raw, facts.unwrap()));
        }

        // A floating result, so the source is an integer: the other three combinations were decided
        // by the two calls above.
        auto raw = constantValueOf(opt, operand(unary.value));
        if(!raw) return nullptr;

        F64 value;
        if(bits == 32) {
            auto word = U32(raw.unwrap());
            F32 single;
            copy((const Byte*)&word, (Byte*)&single, 4);
            value = F64(single);
        } else {
            auto pattern = raw.unwrap();
            copy((const Byte*)&pattern, (Byte*)&value, 8);
        }

        if(!isFoldableFloat(value)) return nullptr;
        return makeFloatConstant(opt, inst, resultType(), value);
    }

    /*
     * A `convert` of a constant - the *value* crossing, at whichever pair of numbers the two ends are.
     *
     * Integer to integer needs no case for widening: `constantValueOf` reads the source at its own
     * width and sign, so a signed one arrives sign-extended, and `narrowToWidth` cuts it back to the
     * result's.
     *
     * **Float to integer is the one that is bounded.** The machine truncates toward zero, and what it
     * does with a value the integer type cannot hold is the target's answer - `cvttsd2si` produces
     * the integer indefinite value, and this stage has made no ruling that says otherwise. So the
     * fold is offered only where the truncated value fits, and out of range the conversion stays and
     * the machine answers it. The shipping stage folds those because it *has* ruled - saturation, in
     * `saturationRange` - and the two agree on the range where anything is folded at all, which is
     * what `FoldFloat.yana`'s shared `.run.expect` says.
     */
    Value* foldConvert(InstUnary& unary) {
        auto sourceType = typeOf(unary.value);
        if(sourceType == resultType()) return operand(unary.value);

        auto fromFloat = foldableFloat(opt, sourceType);
        auto toFloat = foldableFloat(opt, resultType());

        if(toFloat) {
            if(fromFloat) {
                auto source = constantFloatOf(opt, operand(unary.value));
                if(!source || !isFoldableFloat(source.unwrap())) return nullptr;

                return makeFloatConstant(opt, inst, resultType(), source.unwrap());
            }

            // An integer source, read at its own width and sign - which is what decides whether the
            // bits `constantValueOf` handed back denote a negative number.
            auto facts = factsOf(sourceType);
            if(!facts) return nullptr;

            auto source = constantValueOf(opt, operand(unary.value));
            if(!source) return nullptr;

            auto value = facts.unwrap().isSigned ? F64(I64(source.unwrap())) : F64(source.unwrap());
            return makeFloatConstant(opt, inst, resultType(), value);
        }

        auto facts = factsOf(resultType());
        if(!facts) return nullptr;

        if(!fromFloat) {
            auto source = constantValueOf(opt, operand(unary.value));
            if(!source) return nullptr;

            return constant(resultType(), narrowToWidth(source.unwrap(), facts.unwrap()));
        }

        auto source = constantFloatOf(opt, operand(unary.value));
        if(!source || !isFoldableFloat(source.unwrap())) return nullptr;

        auto bits = U32(facts.unwrap().bits);
        auto isSigned = facts.unwrap().isSigned;
        auto value = source.unwrap();

        // Truncation toward zero, and only inside the range the result type holds - see above. The
        // bounds are compared as doubles, which is exact for every width this reaches: the highest
        // `I64` is not a double, so the comparison is against `2^63` and the value one below it is
        // the one produced.
        auto lowest = isSigned ? -(F64(U64(1) << (bits - 1))) : 0.0;
        auto limit = isSigned ? F64(U64(1) << (bits - 1))
                              : (bits >= 64 ? 18446744073709551616.0 : F64(U64(1) << bits));

        if(!(value >= lowest && value < limit)) return nullptr;

        // Through the *result's* own signedness, which is what makes the host conversion defined:
        // the range above admits values up to 2^64 for an unsigned sixty-four bit result, and no
        // `I64` holds one of those.
        auto truncated = isSigned ? U64(I64(value)) : U64(value);
        return constant(resultType(), narrowToWidth(truncated, facts.unwrap()));
    }

    /*
     * The answers a select has that need no arithmetic, and therefore no width to do it at.
     *
     * A decided condition is the one that pays and if-conversion is what will create the shape: an
     * arm that folded to a constant becomes a select whose condition then folds, and without this the
     * branch that `cleanCfg` would have removed survives as a conditional move on a known flag.
     */
    /*
     * A `map` at a selector this pass already knows - the arm's own constant, standing where the
     * table was.
     *
     * One arm and no second opinion: `simplify` calls this and every pass afterwards sees the
     * result, so an inlined `nameOf(Red)` is the string and not a table read. The lookup is linear
     * over the entries, which is what the recognition rule's bound makes free.
     *
     * The **fallback** answers where the domain is open and the selector is outside it, which is the
     * one case a chain would have reached through a comparison. There is nothing to prove about a
     * closed one: recognition covered every constructor, so a selector that matched no entry is a
     * value of a type the pivot cannot hold.
     */
    Value* foldMap(InstMap& map) {
        auto found = constantValueOf(opt, operand(map.selector));
        if(!found) return nullptr;

        auto selector = I64(found.unwrap());

        for(auto entry: map.entries.contents(opt.base)) {
            if(entry.selector != selector) continue;

            return makeConstantValue(opt, inst, entry.value);
        }

        if(map.closed) return nullptr;

        return makeConstantValue(opt, inst, map.fallback);
    }

    Value* foldSelect(InstSelect& select) {
        // Both arms one value is that value, whatever the condition did. This is what a diamond whose
        // two sides computed one thing leaves - and unlike the block argument it came from, it is one
        // instruction, so saying so is a replacement rather than a CFG rewrite.
        if(sameOperand(select.whenTrue, select.whenFalse)) return operand(select.whenTrue);

        auto whenTrue = constantValueOf(opt, operand(select.whenTrue));
        auto whenFalse = constantValueOf(opt, operand(select.whenFalse));

        /*
         * And two arms that are one *number*, which is what two arms producing the same literal
         * leave: the conversion mints a constant per site, so the two alternatives were never the
         * same value even where they were always the same answer.
         *
         * Integers only. Two floating constants that compare equal are not always interchangeable -
         * `0.0` and `-0.0` are one comparison and two values - and choosing between them is not
         * something a fold gets to do silently.
         */
        if(whenTrue && whenFalse && whenTrue.unwrap() == whenFalse.unwrap()) {
            return operand(select.whenTrue);
        }

        /*
         * And `c ? True : False`, which is `c`.
         *
         * Here rather than with the `Bool` rewrite in simplify.cpp for the reason that file gives:
         * this one answers with a value that is already standing there and every rule there needs an
         * instruction that does not exist yet. Both the result and the condition are checked against
         * `Bool`, because `Select` is also the lane-wise one - a vector select's condition is a mask
         * and its arms are vectors, and neither the arms being 0 and 1 nor the condition being the
         * answer is true of one.
         */
        auto boolean = mir::booleanType(opt.program);
        if(whenTrue && whenFalse && resultType() == boolean && typeOf(select.condition) == boolean) {
            if(whenTrue.unwrap() == 1 && whenFalse.unwrap() == 0) return operand(select.condition);
        }

        auto condition = constantValueOf(opt, operand(select.condition));
        if(!condition) return nullptr;

        return condition.unwrap() ? operand(select.whenTrue) : operand(select.whenFalse);
    }

    Value* run() {
        switch(inst.kind) {
            case Inst::Select:
                return foldSelect((InstSelect&)inst);

            case Inst::Map:
                return foldMap((InstMap&)inst);

            case Inst::Unary: {
                auto& unary = (InstUnary&)inst;

                // A reinterpretation is decided ahead of the integer gate, because it is not
                // arithmetic: `foldableInt` would admit it and then it would be declined again for a
                // different reason.
                if(unary.op == UnaryOp::Bitcast) return foldBitcast(unary);

                /*
                 * A conversion is decided ahead of it in part, and §J63 is where "in part" was read
                 * back.
                 *
                 * The identity and the two float crossings compute nothing and have no width to wrap
                 * at, which is what this arm was written for. The **integer to integer** one does
                 * have one - it puts the source into the result's width, through `factsOf` and
                 * `narrowToWidth` - and running it outside `foldAtWidths` meant `factsOf` was asked
                 * with no run in progress and declined every target-answered result. So
                 * `convert (const 5) :: Size` folded nowhere, and that one instruction is what stood
                 * between every string literal and its own length: `stringLiteral` widens the count
                 * to a `Size` on the way into the view record, and a length that is a `convert`
                 * rather than a number is a length no branch above a use can be decided by.
                 *
                 * Through the width rule, so the answer is written down only where the two widths
                 * agree. For a literal that always holds: `mir::integerHolds` is what made it a value
                 * of the type on every target before it was ever written down.
                 */
                if(unary.op == UnaryOp::Convert) {
                    if(typeOf(unary.value) == resultType()) return operand(unary.value);

                    if(foldableFloat(opt, resultType()) || foldableFloat(opt, typeOf(unary.value))) {
                        return foldConvert(unary);
                    }

                    return foldAtWidths([&](const IntFacts&) { return foldConvert(unary); },
                                        resultType());
                }

                /*
                 * `byteSwap(byteSwap(x))` is `x` - the one identity of this operation, and ahead of
                 * the constant gate because it holds for any operand at all.
                 *
                 * Not left to CSE: the two reversals are different values however identical their
                 * operands, and what makes the pair the identity is the operation's own algebra
                 * rather than a repeated computation. Guarded on the two agreeing about the type, so
                 * that a reversal of a reinterpretation of a reversal is left alone.
                 */
                if(unary.op == UnaryOp::ByteSwap) {
                    if(auto producer = operand(unary.value)->definer) {
                        auto& before = *opt.base[producer];
                        if(before.kind == Inst::Unary &&
                           ((InstUnary&)before).op == UnaryOp::ByteSwap) {
                            auto inner = operand(((InstUnary&)before).value);
                            if(inner->type() == resultType()) return inner;
                        }
                    }
                }

                return foldAtWidths([&](const IntFacts& at) { return foldUnary(unary, at); },
                                    resultType());
            }

            case Inst::Binary: {
                auto& binary = (InstBinary&)inst;
                if(mir::isComparison(binary.op)) return foldCompare(binary);

                // Ahead of the integer gate, because a floating result has no `IntFacts` and the gate
                // would end the walk before the arithmetic below is reached.
                if(foldableFloat(opt, resultType())) return foldFloatBinary(binary);

                if(auto folded = foldAtWidths([&](const IntFacts& at) {
                       return foldBinary(binary, at);
                   }, resultType())) {
                    return folded;
                }

                /*
                 * Behind the table, which has first claim: an identity answers with an operand that
                 * is already there where this makes an instruction, and `x * 1 * 4` should reach
                 * `x * 4` by way of `x * 4` rather than by way of a second multiply.
                 *
                 * `foldableInt` rather than `foldAtWidths`, so a **target-answered** width is
                 * declined here where the table above admits one. Reassociation makes an
                 * *instruction* rather than answering a number, and the two trial runs are what the
                 * width rule is built on - a rule that edits cannot be tried. What it costs is one
                 * shape at a `Size`, and the arithmetic it would have folded is folded one tier
                 * down where the width is a number.
                 */
                auto facts = foldableInt(opt, resultType());
                if(!facts) return nullptr;

                return reassociate(binary, facts.unwrap());
            }

            /*
             * `Metric` is deliberately not here, and it is the fourth case of rule two.
             *
             * What a type measures is the representation layer's answer, and this stage runs once per
             * program rather than once per target - the same reason a target-answered width declines
             * above. `lower/inst.cpp` turns one into an immediate where the target is in hand, and
             * `optimizeLowerFunction` folds the arithmetic over it there.
             */
            default:
                return nullptr;
        }
    }
};

} // namespace

Maybe<U64> constantValueOf(OptContext& opt, Value* value) {
    auto constant = constantOf(opt, value);
    if(!constant) return Nothing();

    /*
     * An integer at its own width, and a payload-free constructor read without any width at all.
     *
     * The second is why this is not simply `foldableInt` plus a read: what such a value *is* is a
     * constructor index - a small non-negative number in every register that could hold it - so there
     * is nothing for a width to decide. `Bool` goes through the first arm rather than this one,
     * because `foldableInt` gives it a width and the connectives need it.
     */
    if(constant->kind == mir::Constant::Int) {
        /*
         * A **target-answered** width is read here where `foldableInt` declines it, and reading is
         * not folding: a stored constant of one already holds at the guaranteed width, because a
         * written literal has to be a value of the type on every target (`mir::integerHolds`) and a
         * folded one was checked to be (`mir::integerAnswer`, and `Folder::foldAtWidths` at this
         * tier). So the number is the same number on every target, and normalizing it at the
         * guaranteed width leaves it alone rather than choosing a width for it.
         */
        auto facts = foldableIntWidths(opt, constant->type);
        if(!facts) return Nothing();

        return Just(narrowToWidth(constant->asInt(), facts.unwrap().narrow));
    }

    if(constant->kind == mir::Constant::Enum) return Just(U64(constant->asConstructor()));

    return Nothing();
}

Maybe<F64> constantFloatOf(OptContext& opt, Value* value) {
    auto constant = constantOf(opt, value);
    if(!constant || constant->kind != mir::Constant::Float) return Nothing();

    /*
     * At the type's own width, which for an `F32` means the double this holds read back as a single.
     *
     * `mir::Constant::Float` stores the IEEE pattern of a `double` whatever the declared width is -
     * the narrowing happens where the bytes are laid out - so a constant of an `F32` may hold a
     * number no `F32` has. Reading it at the declared width here is what makes a chain of single
     * precision operations fold to the value the machine computes rather than to a double result
     * rounded once at the end.
     */
    auto value64 = constant->asFloat();
    auto width = foldableFloat(opt, constant->type);

    return Just(width && width.unwrap() == 32 ? F64(F32(value64)) : value64);
}

Maybe<U64> binaryOfConstants(OptContext& opt, InstBinary& binary, U64 left, U64 right) {
    if(!binary.results.size() || !binary.lhs.isSet()) return Nothing();

    auto operandType = opt.base[binary.lhs.value()]->type();
    auto resultType = opt.base[binary.results.get(opt.base, 0)]->type();

    if(mir::isComparison(binary.op)) {
        // A float is not ordered by its pattern, and this reads patterns. `foldCompare` is where the
        // other six live.
        if(foldableFloat(opt, operandType)) return Nothing();

        auto isSigned = compareSignedness(opt, operandType);
        return Just(U64(mir::compareIntegers(binary.op, left, right, isSigned) ? 1 : 0));
    }

    if(foldableFloat(opt, resultType)) return Nothing();

    auto widths = foldableIntWidths(opt, resultType);
    if(!widths) return Nothing();

    // One run at one width, through the rule `Folder::foldBinary` writes its answer with.
    auto at = [&](IntFacts facts) { return evaluateAtWidth(binary.op, facts, left, right); };

    auto narrow = at(widths.unwrap().narrow);
    if(!widths.unwrap().differ()) return narrow;

    // And the width rule: a target-answered type answers only where both widths agree - `IntWidths`
    // in `pass.h` is where what that buys and what it costs are set out.
    auto wide = at(IntFacts { widths.unwrap().wide, widths.unwrap().narrow.isSigned });
    if(!narrow || !wide || narrow.unwrap() != wide.unwrap()) return Nothing();

    return narrow;
}

Value* makeConstant(OptContext& opt, Inst& at, DeclPtr<mir::Type> type, U64 value) {
    auto editor = opt.ir();
    editor.setInsertBefore(at);

    // An integer, or the constructor index of a payload-free enum - which is what a `Bool` is in this
    // stage, and the only kind of enum constant a fold here produces.
    auto constant = opt.decl[type]->kind == mir::Type::Int
        ? mir::intConstant(type, value)
        : mir::enumConstant(type, U16(value));

    return editor.resultOf(*editor.addConst(at.source, constant));
}

/*
 * Any constant at all as a value - the one a `map` arm answers, whatever kind it is.
 *
 * `makeConstant` above takes a number because every other fold produces one; a table's arm may be a
 * float, a string or a record, and what it is was settled where the mapping was recognized. So this
 * installs the constant it was given rather than rebuilding one.
 */
Value* makeConstantValue(OptContext& opt, Inst& at, const mir::Constant& constant) {
    auto editor = opt.ir();
    editor.setInsertBefore(at);

    return editor.resultOf(*editor.addConst(at.source, constant));
}

Value* makeFloatConstant(OptContext& opt, Inst& at, DeclPtr<mir::Type> type, F64 value) {
    auto editor = opt.ir();
    editor.setInsertBefore(at);

    // Rounded to the declared width on the way in, so that the stored pattern is the value the target
    // will hold rather than a double that happens to be near it - the same rounding `cvtsd2ss`
    // performs, and what makes each step of an `F32` chain fold at single precision.
    auto width = foldableFloat(opt, type);
    auto rounded = width && width.unwrap() == 32 ? F64(F32(value)) : value;

    return editor.resultOf(*editor.addConst(at.source, mir::floatConstant(type, rounded)));
}

Value* foldInst(OptContext& opt, Inst& inst) {
    // A folded instruction defines exactly one value, and every kind below does. A kind that defines
    // none has nothing for a fold to answer with, and one that defines several is a call or a
    // compare-exchange, neither of which is arithmetic.
    if(inst.results.size() != 1) return nullptr;

    Folder folder { opt, inst };
    return folder.run();
}

} // namespace ssa
