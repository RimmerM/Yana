#include "opt_pass.h"

/*
 * Constant folding and the algebraic identities.
 *
 * Two rules govern everything here, and both are about the two targets agreeing rather than about
 * arithmetic:
 *
 *  1. **A folded result is stored as the register would have held it.** Native leaves a signed
 *     narrow value sign-extended (`truncateToWidth`) and an unsigned one masked; JS reads a constant
 *     back by sign-extending from the type's own `bits` (`constantValue`). `narrowToWidth` produces
 *     the one form both of those accept, so a folded constant is not a third opinion about what the
 *     value is. `makeFloatConstant` is the same rule for the two floating widths.
 *
 *  2. **Where the targets could disagree about the operation itself, it is not folded.** Every case
 *     is named at the point it is declined, and there are now five: `@bits` refinement *arithmetic*,
 *     whose width the two spell differently; a shift by a distance the type cannot hold, where
 *     each target masks the count its own way; `not` on an *unsigned* type narrower than its
 *     register, which native does not wrap and JS does; a float-to-integer conversion of a value the
 *     integer type cannot hold, where one target produces the integer indefinite value and the other
 *     wraps; and a conversion with a `Bool` at either end, which on JS is a comparison against zero
 *     rather than a truncation. The point of declining rather than choosing is that this pass must
 *     not be the thing that decides which target is right.
 *
 *     The first of those is about arithmetic and nothing else, which is worth saying because two
 *     things here do read a refinement: `constantValueOf` on the way in and `conversionFacts` on the
 *     way out. What `x + 1` wraps at on a `@bits(3) U32` is a question the targets answer
 *     differently; which bits a *conversion* keeps and what sits above them is one they already
 *     answer the same way, and declining that one too would leave a field whose every input was a
 *     literal reaching both backends as arithmetic.
 *
 * Division and remainder decline a zero divisor for the ordinary reason - the program is entitled to
 * whatever the machine does with it, and that is not a constant this pass may invent - and signed
 * division declines the one overflowing pair for the same reason.
 *
 * The floating side declines one more thing, and for a different reason: a NaN or an infinity is a
 * value both targets have and *neither emitter can write*, so a fold that produced one would have
 * nowhere to put it. That is a limit of the notation rather than a disagreement, and it is why
 * `isFoldableFloat` guards the operands as well as the results.
 */

namespace {

struct Folder {
    OptContext& opt;

    ModulePtr<Value> constant(Value& at, TypePtr type, U64 value) {
        return makeConstant(opt, at, type, value);
    }

    // The two operands of a binary instruction as raw values at their own width, or nothing if
    // either is not a constant.
    bool operands(InstBinary& instruction, U64& lhs, U64& rhs) {
        auto left = constantValueOf(opt, instruction.lhs);
        if(!left) return false;

        auto right = constantValueOf(opt, instruction.rhs);
        if(!right) return false;

        lhs = left.unwrap();
        rhs = right.unwrap();
        return true;
    }

    bool isConstantValue(ModulePtr<Value> value, U64 wanted) {
        auto found = constantValueOf(opt, value);
        return found && found.unwrap() == wanted;
    }

    /*
     * Whether the two operands are the same SSA value, which is what makes `x - x` and `x ^ x` zero
     * without either being a constant.
     *
     * Value identity is enough and purity is not required: this is SSA, so one value is one
     * evaluation however expensive it was. `sub %v, %v` where `%v` is a call result is still zero,
     * and the call still happens - what goes away is the subtraction, not its operand.
     */
    bool sameOperand(ModulePtr<Value> a, ModulePtr<Value> b) {
        return a && a == b;
    }

    /*
     * The comparison, at the operands' own signedness.
     *
     * The operand type rather than the result type, which is `Bool` and says nothing about how the
     * two sides are ordered. A signed narrow value arrives here sign-extended to 64 bits by
     * `narrowToWidth`, so comparing as I64 is comparing at the type's own width.
     */
    bool compare(CompareOp op, U64 lhs, U64 rhs, bool isSigned) {
        if(isSigned) {
            auto a = I64(lhs), b = I64(rhs);
            switch(op) {
                case CompareOp::Eq: return a == b;
                case CompareOp::Ne: return a != b;
                case CompareOp::Gt: return a > b;
                case CompareOp::Ge: return a >= b;
                case CompareOp::Lt: return a < b;
                case CompareOp::Le: return a <= b;
            }
        }

        switch(op) {
            case CompareOp::Eq: return lhs == rhs;
            case CompareOp::Ne: return lhs != rhs;
            case CompareOp::Gt: return lhs > rhs;
            case CompareOp::Ge: return lhs >= rhs;
            case CompareOp::Lt: return lhs < rhs;
            case CompareOp::Le: return lhs <= rhs;
        }

        return false;
    }

    // The same six orderings over doubles. IEEE 754 specifies each of them exactly and both targets
    // use the hardware's, so unlike the integer case there is nothing here about width or sign - and
    // nothing about NaN either, because a constant that is one is declined before it arrives.
    bool compareFloat(CompareOp op, F64 lhs, F64 rhs) {
        switch(op) {
            case CompareOp::Eq: return lhs == rhs;
            case CompareOp::Ne: return lhs != rhs;
            case CompareOp::Gt: return lhs > rhs;
            case CompareOp::Ge: return lhs >= rhs;
            case CompareOp::Lt: return lhs < rhs;
            case CompareOp::Le: return lhs <= rhs;
        }

        return false;
    }

    // Which way one of the six orderings answers about a value and itself.
    bool compareReflexive(CompareOp op) {
        return op == CompareOp::Eq || op == CompareOp::Ge || op == CompareOp::Le;
    }

    // A comparison of two constants. Kept apart from the arithmetic because its *result* type is
    // Bool while everything it decides is a question about the operand type.
    ModulePtr<Value> foldCompare(InstCmp& instruction) {
        auto operandType = opt.local[instruction.lhs]->type;
        auto isFloat = foldableFloat(opt, operandType);

        /*
         * The same SSA value on both sides, answered ahead of the type gate.
         *
         * An ordering is reflexive whatever the operands are made of, so this needs neither a width
         * nor a signedness - which is the point of doing it here rather than in the integer path
         * below. A *pointer* is the case that pays: `compare` inlined against one operand twice is
         * `cmp_eq %a, %a` and `cmp_gt %a, %a` over a `Ptr`, which `foldableInt` declines outright,
         * and the two of them are six blocks and a three-way phi in `Instance.yana`.
         *
         * Floats are excluded, and for the one reason IEEE 754 gives: a NaN is not equal to itself,
         * so none of the six is reflexive over the type as a whole.
         */
        if(!isFloat && sameOperand(instruction.lhs, instruction.rhs)) {
            return constant(instruction, instruction.type, compareReflexive(instruction.cmp) ? 1 : 0);
        }

        if(isFloat) {
            auto left = constantFloatOf(opt, instruction.lhs);
            auto right = constantFloatOf(opt, instruction.rhs);
            if(!left || !right) return nullptr;
            if(!isFoldableFloat(left.unwrap()) || !isFoldableFloat(right.unwrap())) return nullptr;

            auto answer = compareFloat(instruction.cmp, left.unwrap(), right.unwrap());
            return constant(instruction, instruction.type, answer ? 1 : 0);
        }

        /*
         * The width the comparison happens at, or nothing where nothing may be decided.
         *
         * A payload-free enum is admitted without one, for the reason `constantValueOf` reads a
         * constant of one without one: what such a value is, is a constructor index - a small
         * non-negative number in every register that could hold it - so all six orderings answer the
         * same however wide the target makes it and whichever signedness it picks. There is nothing
         * for `IntFacts` to say and nothing the two targets could disagree about.
         *
         * `Ordering` is what wants it, and every default `Ord` writes is downstream. `a < b` on an
         * instance that supplies only `compare` is `compare(a, b) == LT`, so a comparison of two
         * constants inlines to `cmp_eq LT, LT` - two constructor constants at an enum type, which
         * every rule above this one declines. Until this was here, all four of Ord's defaults left a
         * live branch on a condition that was already decided: `Default.yana` emitted five of them
         * in one function.
         */
        auto facts = foldableInt(opt, operandType);
        if(!facts && !isConstructorIndex(opt, operandType)) return nullptr;

        U64 lhs, rhs;
        if(!operands(instruction, lhs, rhs)) return nullptr;

        auto isSigned = facts && facts.unwrap().isSigned;
        auto answer = compare(instruction.cmp, lhs, rhs, isSigned);
        return constant(instruction, instruction.type, answer ? 1 : 0);
    }

    /*
     * The identities, each written once and against the *right* operand only.
     *
     * That is what `commute` below buys, and it is the reason the mirror images are gone rather than
     * an accident of which ones anyone happened to write: a commutative operation with a constant on
     * the left either has one on the right too - in which case `known` answers it - or is swapped,
     * and the next round of the fixed point reads it here in the one form. Removing the seven mirrors
     * changed nothing in any of the 82 fixtures, which is what a canonical form is supposed to mean.
     */
    ModulePtr<Value> foldBinary(InstBinary& instruction, const IntFacts& facts) {
        U64 lhs = 0, rhs = 0;
        auto known = operands(instruction, lhs, rhs);

        auto wrap = [&](U64 value) { return constant(instruction, instruction.type, narrowToWidth(value, facts)); };
        auto zero = [&] { return wrap(0); };

        switch(instruction.kind) {
            case Value::Add:
                if(known) return wrap(lhs + rhs);
                if(isConstantValue(instruction.rhs, 0)) return instruction.lhs;
                break;
            case Value::Sub:
                if(known) return wrap(lhs - rhs);
                if(isConstantValue(instruction.rhs, 0)) return instruction.lhs;
                if(sameOperand(instruction.lhs, instruction.rhs)) return zero();
                break;
            case Value::Mul:
                if(known) return wrap(lhs * rhs);
                if(isConstantValue(instruction.rhs, 1)) return instruction.lhs;
                if(isConstantValue(instruction.rhs, 0)) return zero();
                break;
            case Value::Div:
                if(known && rhs != 0) {
                    if(facts.isSigned) {
                        // The one pair whose quotient the type cannot hold. Left to the machine,
                        // which is entitled to trap on it.
                        if(!(I64(rhs) == -1 && I64(lhs) == minLimit<I64>)) return wrap(U64(I64(lhs) / I64(rhs)));
                    } else {
                        return wrap(lhs / rhs);
                    }
                }
                if(isConstantValue(instruction.rhs, 1)) return instruction.lhs;
                break;
            case Value::Rem:
                if(known && rhs != 0) {
                    if(facts.isSigned) {
                        if(!(I64(rhs) == -1 && I64(lhs) == minLimit<I64>)) return wrap(U64(I64(lhs) % I64(rhs)));
                    } else {
                        return wrap(lhs % rhs);
                    }
                }
                break;
            case Value::Shl:
                // A distance the type itself can hold, which is the range every target's shift
                // agrees on: past it, x86 masks the count to the register's width and JS masks it
                // to five bits, and the two answers are the machine's rather than the language's.
                if(known && rhs < facts.bits) return wrap(lhs << rhs);
                if(isConstantValue(instruction.rhs, 0)) return instruction.lhs;
                break;
            case Value::Shr:
                // Logical, so the operand is read as its storage rather than as its value: a signed
                // narrow value is held sign-extended, and shifting that right brings the register's
                // own sign bits down. The same masking `zeroExtendsShiftOperand` emits on native.
                if(known && rhs < facts.bits) {
                    auto mask = facts.bits >= 64 ? ~U64(0) : (U64(1) << facts.bits) - 1;
                    return wrap((lhs & mask) >> rhs);
                }
                if(isConstantValue(instruction.rhs, 0)) return instruction.lhs;
                break;
            case Value::Sar:
                if(known && rhs < facts.bits) {
                    return wrap(facts.isSigned ? U64(I64(lhs) >> rhs) : lhs >> rhs);
                }
                if(isConstantValue(instruction.rhs, 0)) return instruction.lhs;
                break;
            case Value::And:
                if(known) return wrap(lhs & rhs);
                if(isConstantValue(instruction.rhs, 0)) return zero();
                if(sameOperand(instruction.lhs, instruction.rhs)) return instruction.lhs;
                if(isConstantValue(instruction.rhs, narrowToWidth(~U64(0), facts))) return instruction.lhs;
                break;
            case Value::Or:
                if(known) return wrap(lhs | rhs);
                if(isConstantValue(instruction.rhs, 0)) return instruction.lhs;
                if(sameOperand(instruction.lhs, instruction.rhs)) return instruction.lhs;
                break;
            case Value::Xor:
                if(known) return wrap(lhs ^ rhs);
                if(isConstantValue(instruction.rhs, 0)) return instruction.lhs;
                if(sameOperand(instruction.lhs, instruction.rhs)) return zero();
                break;
            default:
                break;
        }

        return nullptr;
    }

    ModulePtr<Value> foldUnary(InstUnary& instruction, const IntFacts& facts) {
        auto from = constantValueOf(opt, instruction.from);
        if(!from) return nullptr;

        switch(instruction.kind) {
            case Value::Neg:
                return constant(instruction, instruction.type, narrowToWidth(U64(0) - from.unwrap(), facts));
            case Value::Not:
                /*
                 * Every type except an *unsigned* one narrower than its register.
                 *
                 * `not` is the one bitwise operation that takes an in-range operand out of range,
                 * and it is not in `wrapsAtDeclaredWidth` - so native leaves `~(0 :: U8)` as
                 * 0xFFFFFFFF in a 32-bit register while JS coerces it to 255. Which of those the
                 * language means is a real question, and answering it by quietly folding to one of
                 * them would settle it in the wrong place.
                 *
                 * A *signed* narrow type is not that question, and the reason is that complement
                 * and sign extension commute: a value of one is held sign-extended - which is what
                 * `truncateToWidth` leaves and what `narrowToWidth` reproduces - and flipping every
                 * bit of `sext(v)` flips the replicated sign bits with the rest, which is `sext(~v)`.
                 * So native's unwrapped `~` on the register already *is* the type's own value, and
                 * it is the value JS reaches by masking the top half and re-signing. `WideInt` is
                 * the case that makes this worth having: `not` is a leaf of every bitwise tree over
                 * one, and declining it left a chain of literals as four instructions and a compare.
                 */
                if(!facts.fillsRegister() && !facts.isSigned) return nullptr;
                return constant(instruction, instruction.type, narrowToWidth(~from.unwrap(), facts));
            default:
                return nullptr;
        }
    }

    /*
     * A commutative operation with its constant on the left, put on the right.
     *
     * One canonical form, which is what lets three separate rules each be written once: `reassociate`
     * below reads `instruction.rhs` for the outer constant, the identities in `foldBinary` above are
     * written against the right operand alone, and CSE in opt_value.cpp unifies `1 + x` with `x + 1`
     * for free once both are spelled the same.
     *
     * Only where the right operand is *not* also a constant, or this would swap a foldable pair back
     * and forth forever - the driver's round cap would turn that into a slow compile rather than a
     * hang, which is not a reason to write it.
     *
     * The operand set is unchanged, so no use list moves: both values still have exactly one entry
     * for this instruction, which is what makes swapping two fields the whole of the rewrite.
     */
    bool commute(InstBinary& instruction) {
        switch(instruction.kind) {
            case Value::Add: case Value::Mul:
            case Value::And: case Value::Or: case Value::Xor:
                break;
            default:
                return false;
        }

        if(!constantValueOf(opt, instruction.lhs)) return false;
        if(constantValueOf(opt, instruction.rhs)) return false;

        auto lhs = instruction.lhs;
        instruction.lhs = instruction.rhs;
        instruction.rhs = lhs;

        opt.changed = true;
        return true;
    }

    /*
     * `(x op c1) op c2` becomes `x op (c1 op c2)`, for the operations where that is an identity.
     *
     * All five are associative *as modular arithmetic*, which is what the type says they are: the two
     * instructions have the same type, so both wrap at the same width and the combined constant wraps
     * with them. Nothing is required of `x` and nothing about how many readers the inner operation
     * has - it stays where it is, and the dead-value pass takes it if this was the last one.
     *
     * Written as a rewrite in place rather than as a replacement value, because what changes is which
     * two operands this instruction has rather than what it computes to. That is also why the use
     * lists are edited by hand here: `Block::add` is what normally records them, and the instruction
     * is already in its block.
     *
     * Not a general reassociation - nothing is reordered and no operation moves between blocks. It
     * exists because the packing expansion produces exactly this shape: a word cleared field by
     * field is one `and` against a literal per field, and nine of them are one mask.
     */
    bool reassociate(ModulePtr<Inst> pointer, InstBinary& instruction, const IntFacts& facts) {
        switch(instruction.kind) {
            case Value::Add: case Value::Mul:
            case Value::And: case Value::Or: case Value::Xor:
                break;
            default:
                return false;
        }

        auto outer = constantValueOf(opt, instruction.rhs);
        if(!outer) return false;

        auto left = opt.local[instruction.lhs];
        if(left->kind != instruction.kind || left->type != instruction.type) return false;

        auto& inner = (InstBinary&)*left;
        auto middle = constantValueOf(opt, inner.rhs);
        if(!middle) return false;

        U64 combined = 0;
        switch(instruction.kind) {
            case Value::Add: combined = middle.unwrap() + outer.unwrap(); break;
            case Value::Mul: combined = middle.unwrap() * outer.unwrap(); break;
            case Value::And: combined = middle.unwrap() & outer.unwrap(); break;
            case Value::Or:  combined = middle.unwrap() | outer.unwrap(); break;
            default:         combined = middle.unwrap() ^ outer.unwrap(); break;
        }

        auto value = makeConstant(opt, instruction, instruction.type, narrowToWidth(combined, facts));

        dropUse(opt, instruction.lhs, pointer);
        instruction.lhs = inner.lhs;
        opt.local[instruction.lhs]->uses.push(opt.program.arena, pointer);

        dropUse(opt, instruction.rhs, pointer);
        instruction.rhs = value;
        opt.local[value]->uses.push(opt.program.arena, pointer);

        opt.changed = true;
        return true;
    }

    /*
     * How many bits a conversion of an integer keeps, refinements included.
     *
     * Deliberately not `foldableInt`, which declines a `@bits` refinement because the two targets
     * spell its *arithmetic* width differently. A conversion asks a narrower question - which bits
     * survive - and both targets answer that one with the type's own `bits`, since a store to a
     * refined field masks to exactly that many on either side. `Bool` is one bit for the same
     * reason `foldableInt` says so.
     */
    Maybe<U16> conversionBits(TypePtr type) {
        if(!type) return Nothing();
        if(type == opt.program.scalar.bool_) return Just(U16(1));
        if(opt.global[type]->kind != Type::Int) return Nothing();

        auto bits = ((IntType*)opt.global[type])->bits;
        return bits == 0 || bits > 64 ? Nothing() : Just(bits);
    }

    /*
     * The same question with the sign attached, which is what converting a *constant* to a type
     * needs: `narrowToWidth` decides the bits above `bits` from it.
     *
     * The refinement is admitted here for the reason `constantValueOf` admits one on the way in.
     * `foldableInt` declines a refinement because the two targets disagree about what `x + 1` wraps
     * at, and that is a question about arithmetic; which bits a conversion keeps and what is above
     * them is not that question, and both targets already answer it the same way - `truncateToWidth`
     * shifts a signed narrow value up and arithmetically back down, `constantValue` in
     * codegen/js/place.cpp sign-extends a constant from the type's own `bits`, and `narrowToWidth`
     * is the one stored form both of those read as the same number.
     */
    Maybe<IntFacts> conversionFacts(TypePtr type) {
        if(auto facts = foldableInt(opt, type)) return facts;
        if(!type || opt.global[type]->kind != Type::Int) return Nothing();

        // Only the refinement is left: everything else foldableInt declined, it declined for a
        // reason that is not about width, and re-deciding one of those here would be this pass
        // holding two opinions about the same type.
        auto integer = (IntType*)opt.global[type];
        if(!integer->canonical) return Nothing();
        if(integer->bits == 0 || integer->bits > 64) return Nothing();

        return Just(IntFacts {
            integer->bits, IntType::registerBits(integer->width), integer->isSigned
        });
    }

    /*
     * A conversion of a constant between two integer readings of it.
     *
     * Both directions are already decided by the two functions this calls, which is why there is no
     * case for widening: `constantValueOf` reads the source at its own width and sign, so a signed
     * one arrives sign-extended, and `narrowToWidth` cuts it back to the target's. Anything that is
     * not an integer at either end fails one of the two and is left alone - a pointer, a float, a
     * sum's payload.
     *
     * A conversion *to* a refinement is what makes this worth being its own function rather than a
     * case in the switch below, which `foldableInt` gates. `p.d = p.d + 1` on a `@bits(4) Int` field
     * is a widening, an add, a shift pair and a conversion back, and the last of those was the one
     * link in the chain that did not fold - so a field whose every input was a literal still reached
     * both backends as arithmetic, and `hostShaped` in `ScalarRecord.yana` multiplied two of them by
     * a constant at runtime.
     */
    ModulePtr<Value> foldConstantCast(InstUnary& instruction) {
        auto facts = conversionFacts(instruction.type);
        if(!facts) return nullptr;

        auto from = constantValueOf(opt, instruction.from);
        if(!from) return nullptr;

        return constant(instruction, instruction.type, narrowToWidth(from.unwrap(), facts.unwrap()));
    }

    /*
     * The two conversions that are not conversions.
     *
     * The first is a `cast` to the type its operand already has. Types are interned, so equal
     * pointers are the same type and there is nothing between the two readings of the value to
     * compute - which is exactly what the packing expansion leaves behind, since it puts a `Cast`
     * at each end of an expanded access and one end is often already the storage unit's own type.
     *
     * The second is a pair of conversions where the middle one is no narrower than the last. Every
     * integer conversion here keeps the low `bits` of its operand and decides the rest from the
     * result type alone, so where the intermediate keeps at least as many bits as the result does,
     * the bits the result is built from are the source's own. Signedness needs no case for the same
     * reason: it decides what is above those bits, and the outer conversion decides that either way.
     *
     * Nothing is required of the inner conversion's other readers. It stays where it is and the
     * dead-value pass collects it if this was the last one.
     */
    bool foldCast(ModulePtr<Inst> pointer, InstUnary& instruction) {
        auto inner = opt.local[instruction.from];
        if(inner->kind != Value::Cast) return false;

        auto middle = conversionBits(inner->type);
        auto result = conversionBits(instruction.type);
        if(!middle || !result || middle.unwrap() < result.unwrap()) return false;

        auto source = ((InstUnary&)*inner).from;
        if(!conversionBits(opt.local[source]->type)) return false;

        dropUse(opt, instruction.from, pointer);
        instruction.from = source;
        opt.local[source]->uses.push(opt.program.arena, pointer);

        opt.changed = true;
        return true;
    }

    /*
     * A conversion with a floating type at one end or both.
     *
     * Three of the four directions need nothing said about them. Widening an integer, narrowing a
     * double to a float and widening a float to a double are all exactly specified, both targets
     * round to nearest, and the host does the same - so the fold is the C++ conversion and the
     * result is stored at the target's width by `makeFloatConstant`.
     *
     * **Float to integer is the one that has to be bounded**, and it is the case Rule 2 of this file
     * exists for. Both targets truncate toward zero, and both do something *different* with a value
     * the integer type cannot hold: `cvttsd2si` produces the integer indefinite value, while JS
     * emits `Math.trunc(v)` followed by the coercion the type needs, which is modular. So the fold
     * is offered only where the truncated value fits, which is the range on which the two agree -
     * the same shape as declining the one signed division whose quotient does not fit.
     *
     * `Bool` is excluded at both ends. A conversion *to* one is not a truncation on JS at all - it
     * is `value !== 0`, so `2.5` becomes `true` where truncating to one bit would give `0` - and
     * that is a disagreement about what the language means rather than about arithmetic, which this
     * pass may not settle. Nothing produces such a conversion today; declining it costs a test.
     */
    ModulePtr<Value> foldNumericCast(InstUnary& instruction) {
        auto sourceType = opt.local[instruction.from]->type;
        auto fromFloat = foldableFloat(opt, sourceType);
        auto toFloat = foldableFloat(opt, instruction.type);

        if(!fromFloat && !toFloat) return nullptr;
        if(sourceType == opt.program.scalar.bool_) return nullptr;
        if(instruction.type == opt.program.scalar.bool_) return nullptr;

        if(toFloat) {
            if(auto source = constantFloatOf(opt, instruction.from)) {
                if(!isFoldableFloat(source.unwrap())) return nullptr;
                return makeFloatConstant(opt, instruction, instruction.type, source.unwrap());
            }

            // An integer source, read at its own width and sign - which is what decides whether the
            // bits `constantValueOf` handed back denote a negative number.
            auto facts = foldableInt(opt, sourceType);
            if(!facts) return nullptr;

            auto source = constantValueOf(opt, instruction.from);
            if(!source) return nullptr;

            auto value = facts.unwrap().isSigned ? F64(I64(source.unwrap())) : F64(source.unwrap());
            return makeFloatConstant(opt, instruction, instruction.type, value);
        }

        auto facts = foldableInt(opt, instruction.type);
        if(!facts) return nullptr;

        auto source = constantFloatOf(opt, instruction.from);
        if(!source || !isFoldableFloat(source.unwrap())) return nullptr;

        /*
         * In range for an `I64` first, so that the truncation itself is defined - and then in range
         * for the type actually being converted to. Two steps rather than one because the bound for
         * the second is an integer bound, and comparing a double against `2^63 - 1` is comparing it
         * against `2^63`, which is a different question.
         */
        constexpr F64 kTwoPow63 = 9223372036854775808.0;
        auto value = source.unwrap();
        if(value < -kTwoPow63 || value >= kTwoPow63) return nullptr;

        auto truncated = I64(value);
        auto bits = facts.unwrap().bits;

        if(facts.unwrap().isSigned) {
            if(bits < 64) {
                auto limit = I64(1) << (bits - 1);
                if(truncated < -limit || truncated >= limit) return nullptr;
            }
        } else {
            if(truncated < 0) return nullptr;
            if(bits < 64 && U64(truncated) >= (U64(1) << bits)) return nullptr;
        }

        return constant(instruction, instruction.type, narrowToWidth(U64(truncated), facts.unwrap()));
    }

    /*
     * Floating arithmetic over two constants.
     *
     * IEEE 754 specifies add, subtract, multiply and divide exactly, and both targets use the
     * hardware's. `Double` is therefore the host's own `double` and there is nothing to say.
     *
     * `Float` is worth one sentence, because the two targets reach it differently and still agree:
     * natively it is a single-precision instruction, while `codegen/js` computes in a double and
     * rounds with `Math.fround`. Rounding a double result to single is the same value as computing
     * in single throughout - for one operation, and because a double keeps 53 bits where twice a
     * float's 24 plus two is 50 - so the two are the same number and the host's `float` arithmetic
     * is a third spelling of it.
     *
     * A non-finite result is declined rather than folded: dividing by zero is the way to reach one,
     * and neither emitter has a literal for an infinity or a NaN. `Rem` is left alone entirely,
     * since what it means for floats is not a question this needs to have an opinion about.
     */
    // Whether one operand is exactly this value, for the two identities below.
    bool isFloatValue(ModulePtr<Value> value, F64 wanted) {
        auto found = constantFloatOf(opt, value);
        return found && found.unwrap() == wanted;
    }

    ModulePtr<Value> foldFloatBinary(InstBinary& instruction) {
        auto left = constantFloatOf(opt, instruction.lhs);
        auto right = constantFloatOf(opt, instruction.rhs);

        /*
         * The two identities floating arithmetic actually has, and the reason the list is this short.
         *
         * `x * 1` and `x / 1` return their operand unchanged for *every* value: a NaN stays that NaN,
         * an infinity keeps its sign, and `-0` stays negative. That is what an identity has to mean
         * here, because the operand is a value this pass knows nothing about.
         *
         * The ones an integer reader would expect next are all wrong, and are worth naming so that
         * nobody adds them later. `x + 0` is not `x`, because `-0 + 0` is `+0`. `x * 0` is not `0`,
         * because `NaN * 0` is `NaN` and `-1 * 0` is `-0`. `x - x` is not `0`, because an infinity
         * minus itself is `NaN`. Each of those is an identity over the reals and none of them is one
         * over IEEE 754, which is what the type actually is.
         */
        if(!left || !right) {
            if(isFloatValue(instruction.rhs, 1.0)) {
                if(instruction.kind == Value::Mul || instruction.kind == Value::Div) {
                    return instruction.lhs;
                }
            }

            if(instruction.kind == Value::Mul && isFloatValue(instruction.lhs, 1.0)) {
                return instruction.rhs;
            }

            return nullptr;
        }

        if(!isFoldableFloat(left.unwrap()) || !isFoldableFloat(right.unwrap())) return nullptr;

        auto lhs = left.unwrap(), rhs = right.unwrap();
        F64 result;

        switch(instruction.kind) {
            case Value::Add: result = lhs + rhs; break;
            case Value::Sub: result = lhs - rhs; break;
            case Value::Mul: result = lhs * rhs; break;
            case Value::Div:
                if(rhs == 0.0) return nullptr;
                result = lhs / rhs;
                break;
            default:
                return nullptr;
        }

        if(!isFoldableFloat(result)) return nullptr;
        return makeFloatConstant(opt, instruction, instruction.type, result);
    }

    /*
     * Everything this pass does with a floating *result*, which is what the integer path below can
     * say nothing about: `foldableInt` declines the type outright and stops the walk.
     *
     * Negating zero is declined. `-0.0` is a value both targets have and neither emitter writes -
     * `number` prints it as `0`, which is a different value the moment anything divides by it - so
     * this is the one algebraic identity here that is about the *notation* rather than the
     * arithmetic.
     */
    ModulePtr<Value> foldFloat(Value& instruction) {
        if(!foldableFloat(opt, instruction.type)) return nullptr;

        switch(instruction.kind) {
            case Value::Neg: {
                auto source = constantFloatOf(opt, ((InstUnary&)instruction).from);
                if(!source || !isFoldableFloat(source.unwrap()) || source.unwrap() == 0.0) return nullptr;

                return makeFloatConstant(opt, instruction, instruction.type, -source.unwrap());
            }
            case Value::Add: case Value::Sub: case Value::Mul: case Value::Div:
                return foldFloatBinary((InstBinary&)instruction);
            default:
                return nullptr;
        }
    }

    /*
     * How wide a concrete type is on the target this program is being optimized for.
     *
     * `InstTypeMetric` exists so that layout travels in the IR rather than being decided during
     * resolution, and its header says who is entitled to answer: *"the native path folds it against
     * its own Repr table, and the JS path against its own"*. This stage is one of those paths -
     * `optimizeProgram` takes a `ReprTarget` and builds the table for it, and a JS build and a
     * native build never share a program - so folding it here is that same answer given one stage
     * earlier, not a second opinion about it.
     *
     * Earlier is what it is for. Both backends already fold this, and folding it *there* is too
     * late for everything that reads a constant: an element address is `base + index * strideof T`,
     * and until the stride is a number that address is opaque arithmetic, so two accesses to one
     * array cannot be told apart and nothing about an array literal is statically known. See
     * `addressOffsetOf` in opt_place.cpp, which is the reader this exists for.
     *
     * Only a *concrete* type. A generic one has no number here at all - it is a load out of the
     * caller's descriptor, which is `genSlot`'s answer and belongs to the stage that has one.
     */
    ModulePtr<Value> foldMetric(Value& instruction) {
        auto& metric = (InstTypeMetric&)instruction;
        if(!metric.of || isGeneric(opt.global, metric.of)) return nullptr;

        auto& repr = opt.repr.of(metric.of);
        auto number = metric.metric == TypeMetricKind::Align ? repr.align
                    : metric.metric == TypeMetricKind::Stride ? repr.stride
                    : repr.size;

        // At the metric's own type rather than at a word, on rule 1 above: what a backend reads
        // back out of the constant has to be what it would have written into the register.
        auto facts = foldableInt(opt, instruction.type);
        if(!facts) return nullptr;

        return constant(instruction, instruction.type, narrowToWidth(U64(number), facts.unwrap()));
    }

    /*
     * The two answers a select has that need no arithmetic at all, and therefore no width to do it
     * at - which is why this is above the integer gate with the comparison and the metric rather
     * than in the switch below.
     *
     * A decided condition is the one that pays. If-conversion is what *creates* the shape: an arm
     * inlining reduced to a constant becomes a select whose condition then folds, and without this
     * the branch that opt_branch.cpp would have removed survives as a `cmov` on a known flag.
     */
    ModulePtr<Value> foldSelect(InstSelect& select) {
        // Both arms the same value is the same value, whatever the condition did. This is the shape
        // a diamond whose two sides computed one thing leaves - and unlike the phi it came from, it
        // is one instruction, so saying so is a replacement rather than a CFG rewrite.
        if(select.whenTrue == select.whenFalse) return select.whenTrue;

        /*
         * And two arms that are one *number*, which is what two arms producing the same literal
         * leave: a constant belongs to no block and is materialized per use, so the phi's two
         * alternatives were never the same value even where they were always the same answer.
         *
         * Integers only. Two floating constants that compare equal are not always interchangeable -
         * `0.0` and `-0.0` are one comparison and two values - and choosing between them is not
         * something a fold gets to do silently.
         */
        auto whenTrue = constantValueOf(opt, select.whenTrue);
        if(whenTrue) {
            auto whenFalse = constantValueOf(opt, select.whenFalse);
            if(whenFalse && whenTrue.unwrap() == whenFalse.unwrap()) return select.whenTrue;
        }

        auto condition = constantValueOf(opt, select.cond);
        if(!condition) return nullptr;

        return condition.unwrap() ? select.whenTrue : select.whenFalse;
    }

    ModulePtr<Value> fold(ModulePtr<Inst> pointer, Value& instruction) {
        if(instruction.kind == Value::Cmp) return foldCompare((InstCmp&)instruction);
        if(instruction.kind == Value::Select) return foldSelect((InstSelect&)instruction);
        if(instruction.kind == Value::TypeMetric) return foldMetric(instruction);

        /*
         * Every conversion is decided here rather than in the switch below, because none of them is
         * arithmetic: a conversion to a `@bits` refinement is declined by `foldableInt` for a reason
         * that is about what `x + 1` wraps at, and a conversion that computes nothing has no width
         * to wrap at in the first place.
         */
        if(instruction.kind == Value::Cast) {
            auto& cast = (InstUnary&)instruction;
            if(opt.local[cast.from]->type == instruction.type) return cast.from;

            // Before the chain collapse rather than after it, because the three answer different
            // questions about the same instruction and only one of them can apply: `conversionBits`
            // declines a float at either end, so a conversion the first two fold is one that never
            // reaches the collapse and the other way round.
            if(auto folded = foldNumericCast(cast)) return folded;
            if(auto folded = foldConstantCast(cast)) return folded;

            foldCast(pointer, cast);
        }

        // Ahead of the integer gate for the same reason the identity cast is: a floating result has
        // no `IntFacts` and `foldableInt` would end the walk before the switch is reached.
        if(auto folded = foldFloat(instruction)) return folded;

        auto facts = foldableInt(opt, instruction.type);
        if(!facts) return nullptr;

        switch(instruction.kind) {
            case Value::Neg:
            case Value::Not:
                return foldUnary((InstUnary&)instruction, facts.unwrap());
            case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
            case Value::Shl: case Value::Shr: case Value::Sar:
            case Value::And: case Value::Or: case Value::Xor: {
                auto& binary = (InstBinary&)instruction;
                if(auto folded = foldBinary(binary, facts.unwrap())) return folded;

                // Only where nothing else applied, so that a foldable pair is folded rather than
                // rearranged first. Each rewrite leaves an instruction the next round may fold.
                if(!commute(binary)) reassociate(pointer, binary, facts.unwrap());
                return nullptr;
            }
            default:
                return nullptr;
        }
    }
};

}

void foldFunction(OptContext& opt) {
    Folder folder { opt };

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        // Nothing is inserted into or removed from the list here - a replaced instruction is left
        // for the dead-value pass to collect - so a plain index walk sees each instruction once.
        for(Size i = 0; i < block->instructions.size(); i++) {
            auto pointer = block->instructions.get(opt.local, i);
            auto folded = folder.fold(pointer, *opt.local[pointer]);
            if(folded) replaceValue(opt, (ModulePtr<Value>)pointer, folded);
        }

        // A branch on a constant is left alone *here*: removing the edge is a CFG rewrite, and the
        // phis, the dominance and the ownership passes' block-level facts all rest on the shape of
        // the graph. opt_branch.cpp is that rewrite, and runs after place forwarding.
    }
}
