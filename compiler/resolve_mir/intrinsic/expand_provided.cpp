#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The members of the instances the compiler generates - `provided.def`, one arm per row and one
 * case per member of its class, indexed by the position `known.def` states.
 *
 * Each arm is the *whole* definition of what that member computes, read from two places: a call
 * with concrete operands expands to the node here in place of the call, and `provideBodies` writes
 * the same node over the member's parameters so that a call from a generic body - specialized or
 * compiled once - reaches an ordinary function. One statement, two placements, which is what keeps
 * `a + b` in `main` and `x + y` in `fn (Num(a)) f(x: a, y: a)` the same instruction.
 *
 * What is not here is anything about the *language*: which types have these instances is
 * `provided.def`'s family column, and which member a written operator reaches is name resolution
 * and instance selection. An arm answers what it is handed and asks nothing.
 */

/*
 * What a literal folds to - `FromInt(I32).fromInt(5)` is `const 5 :: I32`.
 *
 * The one place a written number becomes a constant of the type a position asked for, shared by
 * the expansion below (a constant argument) and by `Resolver::resolveLiteral` (a constant
 * position, which has no node to hand an expansion). Two readers because the second has nowhere
 * to build the `Long` the call would take; one function because a literal's bits at its type is
 * a fact that has to have one answer.
 *
 * `bits` is the number as two's complement - the magnitude, or its negation where the literal was
 * negative - and `value` is the same number as a double, which is what a float target takes: an
 * integer at a float is what `Num`'s `FromInt` superclass promises every number can do, and an
 * integer is reduced to the width its type declares - see below, which is the one place a constant
 * is put in normal form.
 */
Constant foldLiteral(DeclBase base, DeclPtr<Type> type, U64 bits, double value) {
    if(base[type]->kind == Type::Float) return floatConstant(type, value);

    assertTrue(base[type]->kind == Type::Int);

    // In the type's **normal form** - `reducedToWidth`, where what that is and why a constant is the
    // producer that has to establish it are argued. A written number wider than its type is warned
    // about where it was written; here it is simply the number the type holds.
    return intConstant(type, reducedToWidth(*(IntType*)base[type], bits));
}

/*
 * Whether a member is one node over its operands, or a body of its own.
 *
 * `compare` is the one member of these classes that is not a single instruction: it answers an
 * `Ordering`, which is two comparisons and three constants laid out as control flow. A call to it
 * is therefore an ordinary call to the generated body rather than a graph spliced into every call
 * site - the shipping stage's `generateCompare` makes the same choice for the same reason - and
 * `provideBodies` is where that body is written.
 */
bool providedExpands(Provided role, U16 member) {
    return !(role == Provided::ord && member == kOrdCompare);
}

Node* expandProvided(Builder& builder, Provided role, U16 member, const DeclPtr<Type>* head,
                     Size heads, Node* const* arguments, Size count, LocationId source) {
    // The head's first argument is what a single-headed row is *at*; the last is what a conversion
    // answers with, and for a single-headed row they are the same handle. Named here so no arm
    // subscripts a list whose length depends on its own row.
    auto type = head[0];
    auto answer = head[heads - 1];

    auto binary = [&](BinaryOp op) {
        assertTrue(count == 2);
        return builder.addBinary(source, op, *arguments[0], *arguments[1]);
    };

    auto unary = [&](UnaryOp op) {
        assertTrue(count == 1);
        return builder.addUnary(source, op, *arguments[0]);
    };

    switch(role) {
        /*
         * `fromInt` and `fromDecimal` - a constant argument folds to a constant of the instance's
         * type, and any other argument is the conversion. The fold is what keeps a literal at a
         * primitive the `const` it always was, with no `Long` standing above it in the dump.
         */
        case Provided::fromInt:
        case Provided::fromDecimal: {
            assertTrue(count == 1 && member == 0);
            auto& argument = *arguments[0];

            if(argument.kind == Node::Const) {
                auto written = builder.body.constant(builder.base(), ((NodeConst&)argument).constant);

                // The argument is the declaration's `Long` or `Double`, so the two readings of it
                // are a signed integer and a double - which is what `foldLiteral` takes.
                auto bits = written.kind == Constant::Int ? written.asInt() : U64(0);
                auto value = written.kind == Constant::Int ? double(I64(written.asInt()))
                                                           : written.asFloat();

                return builder.addConst(source, foldLiteral(builder.declBase(), type, bits, value));
            }

            return builder.addConvert(source, argument, type);
        }

        case Provided::eq:
            return binary(member == 0 ? BinaryOp::Eq : BinaryOp::Ne);

        case Provided::ord:
            switch(member) {
                case 0: return binary(BinaryOp::Lt);
                case 1: return binary(BinaryOp::Le);
                case 2: return binary(BinaryOp::Gt);
                case 3: return binary(BinaryOp::Ge);
            }
            break;

        case Provided::num:
            switch(member) {
                case 0: return binary(BinaryOp::Add);
                case 1: return binary(BinaryOp::Sub);
                case 2: return binary(BinaryOp::Mul);
                case 3: return binary(BinaryOp::Div);
                case 4: return unary(UnaryOp::Neg);
            }
            break;

        case Provided::bitwise:
            switch(member) {
                case 0: return binary(BinaryOp::And);
                case 1: return binary(BinaryOp::Or);
                case 2: return binary(BinaryOp::Xor);
                case 3: return unary(UnaryOp::Not);
            }
            break;

        case Provided::integral:
            switch(member) {
                case 0: return binary(BinaryOp::Rem);
                case 1: return binary(BinaryOp::Shl);
                case 2: return binary(BinaryOp::Shr);
                case 3: return binary(BinaryOp::Sar);
                case 4: return binary(BinaryOp::Rol);
                case 5: return binary(BinaryOp::Ror);
            }
            break;

        /*
         * `truthy` - the identity at `Bool`, which is why `if a > b` produces the IR it always
         * did, and "not zero" at a number. The zero is built at the operand's type, which is what
         * makes one arm serve every integer and both floats; `NaN` is therefore truthy, which is
         * `doc/spec/expressions.md`'s rule and not a surprise.
         */
        case Provided::truth: {
            assertTrue(count == 1 && member == 0);
            auto& value = *arguments[0];
            auto from = valueType(value);

            if(from == booleanType(builder.program)) return &value;

            auto zero = builder.addConst(source, builder.declBase()[from]->kind == Type::Float
                                                     ? floatConstant(from, 0.0)
                                                     : intConstant(from, 0));
            return builder.addBinary(source, BinaryOp::Ne, value, *zero);
        }

        /*
         * `widen` and `truncate` - the conversion ladders, and one node each.
         *
         * The result type is the *head's* second argument and never the operand's: which rung this
         * is was decided by instance selection, and an emitter that read it off the value would be
         * deciding it a second time. Widening and narrowing are one node here for the same reason
         * `div` is one node over signed and unsigned operands - what a machine does with the two
         * ends is a question about the widths, and `resolve_ssa/lower_inst.cpp` is where it is
         * asked, with the sign of each side read off its own logical type.
         *
         * The identity rung of the narrowing ladder reaches this too, and builds a `convert` from a
         * type to itself. It is not folded away here: what a conversion between two equal types
         * costs is the manifestation's answer, and this file states what the member computes.
         */
        case Provided::widen:
        case Provided::narrow:
            assertTrue(count == 1 && member == 0);
            return builder.addConvert(source, *arguments[0], answer);

        /*
         * `bitcast` - the same bits, read as another type.
         *
         * A node of its own rather than a `convert`, because the two are different operations that
         * happen to agree on some pairs: a `convert` from `F32` to `I32` is the number rounded, and
         * this is the four bytes read again. No rung relating a pair of different widths was ever
         * generated - `intrinsic::reinterpretable` - so nothing here has to ask whether it fits.
         */
        case Provided::bitcast:
            assertTrue(count == 1 && member == 0);
            return builder.addBitcast(source, *arguments[0], answer);

        case Provided::Count:
            break;
    }

    // A member `provided.def`'s class row says exists and no arm above answers - a mismatch between
    // this file and `known.def`'s member order, which is a mistake in the compiler.
    assertTrue(false);
    return nullptr;
}

} // namespace mir
