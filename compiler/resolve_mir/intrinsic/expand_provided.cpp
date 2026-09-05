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

/*
 * Whether two vector shapes are the same bits under two names - the rule the generated `Bitcast`
 * heads over vectors cannot state.
 *
 * A **vector** keeps its byte width: `i8x16` to `i32x4` is a rung and `i8x16` to `i32x8` is not. A
 * **mask** keeps its lane width *and* its count, which is stricter and is why: two masks of one byte
 * width but different lane shapes have no corresponding lanes, so reinterpreting one as the other
 * would be a repacking rather than a renaming.
 *
 * True for every pair that is not a vector at all, because those reached the arm through a ground
 * head that exists only for pairs the conversion rules relate.
 */
static bool reinterpretableShapes(Builder& builder, DeclPtr<Type> from, DeclPtr<Type> to,
                                  LocationId source) {
    auto base = builder.declBase();
    if(base[from]->kind != Type::Vector || base[to]->kind != Type::Vector) return true;

    auto& read = *(VectorType*)base[from];
    auto& written = *(VectorType*)base[to];

    // A head still holding its own variables - the generated member's body, where the widths are not
    // numbers yet. What decides the pair is the call that specializes it.
    if(!read.hasFixedCount() || !written.hasFixedCount()) return true;

    auto widths = targetIntWidths(builder.context.settings);
    auto fromLane = laneBytes(base, widths, read.lane);
    auto toLane = laneBytes(base, widths, written.lane);
    if(!fromLane || !toLane) return true;

    auto ok = read.isMask || written.isMask
        ? read.isMask && written.isMask && fromLane == toLane && read.lanes == written.lanes
        : fromLane * read.lanes == toLane * written.lanes;

    if(ok) return true;

    builder.context.diagnostics.error(
        read.isMask || written.isMask
            ? "a mask is reinterpreted only at its own shape - %@ and %@ have no corresponding lanes, so reading one as the other is a repacking rather than a renaming"_v
            : "these two vectors are not the same bits - a reinterpretation keeps the byte width, and %@ and %@ do not agree about it"_v,
        source, typeText(builder.context, base, from), typeText(builder.context, base, to));

    return false;
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

            /*
             * At a **vector**, the literal in every lane - `1 :: Vec(I32)` is a legal spelling of
             * `splat(1)`, and it is not a convenience.
             *
             * `class (FromInt(a)) Num(a)` makes the superclass compulsory: a vector with `Num` has
             * to say what an integer literal means as one, and "the literal in every lane" is the
             * only answer that is not arbitrary. So this arm is what `x + 1` inside a body generic
             * over a float type reaches when the substitution was `Vec(F32)`.
             *
             * The lane's own instance answers first, over the same argument - which is what keeps
             * the fold: `splat(const 1)` rather than a splat of a conversion of a `Long`.
             */
            if(isVectorType(builder.declBase(), type)) {
                auto lane = laneTypeOf(builder.declBase(), type);
                auto value = expandProvided(builder, role, member, &lane, 1, arguments, count,
                                            source);

                return value ? builder.addVecSplat(source, *value, type) : nullptr;
            }

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
         * `Bits` - three counts, `bzhi`, and the one member that is not an instruction.
         *
         * The order is `known.def`'s `bits` row and `lib/Core/Bits.yana`'s declaration order, which
         * is one order written in two places on purpose - a member reached by position is a fact
         * about the library, and the row is where the dependency is stated.
         *
         * `bitWidth` is `width - leadingZeros(value)`, with the width written as a constant of the
         * instance's own type. That is what a source body could not have said: a subexpression made
         * only of literals resolves on its own and takes `default FromInt`, so a generic body's
         * `leadingZeros(0)` is an `Int` whatever `a` is. Here the type is in hand.
         *
         * **The width for `Size` is not a number this stage has** - one resolution serves every
         * target - so it is `sizeOf` in bytes shifted up by three. That costs nothing: the metric
         * folds to an immediate wherever the target is known, which is every path that reaches a
         * machine, so the shift is one constant against another and `bitWidth(x: Size)` is still one
         * subtraction. A fixed width is written directly, which folds the whole expression away at a
         * constant argument.
         */
        case Provided::bits:
            switch(member) {
                case 0: return unary(UnaryOp::CountBits);
                case 1: return unary(UnaryOp::LeadingZeros);
                case 2: return unary(UnaryOp::TrailingZeros);
                case 3: return binary(BinaryOp::BitsUpTo);

                case 4: {
                    assertTrue(count == 1);
                    auto& integer = *(IntType*)builder.declBase()[type];

                    auto width = integer.isTargetWidth()
                        ? builder.addBinary(source, BinaryOp::Shl,
                              *builder.addMetric(source, TypeMetric::Size, type, type),
                              *builder.addConst(source, intConstant(type, 3)))
                        : builder.addConst(source, intConstant(type, integer.bits));

                    auto zeros = builder.addUnary(source, UnaryOp::LeadingZeros, *arguments[0]);
                    return builder.addBinary(source, BinaryOp::Sub, *width, *zeros);
                }
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
         * ends is a question about the widths, and `resolve_ssa/lower/inst.cpp` is where it is
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

            /*
             * The one rung whose *shape* is checked here rather than by the head that matched.
             *
             * A reinterpretation over vectors keeps the byte width and is otherwise free, and over
             * masks it keeps the lane width and the count - both are arithmetic relations between
             * two widths and two counts, which no requirement of a head can express. So the vector
             * heads are open in those positions and this is where a pair is held to them:
             * `reinterpretableShapes` is the rule, and it is asked at the call, where the two shapes
             * are concrete and can be named.
             *
             * Every other pair reached this arm through a *ground* head that only exists for pairs
             * `intrinsic::reinterpretable` relates, so there is nothing for it to check.
             */
            if(!reinterpretableShapes(builder, type, answer, source)) return nullptr;

            return builder.addBitcast(source, *arguments[0], answer);

        /*
         * `Lanewise` - the six comparisons and `select`, and seven single instructions.
         *
         * A comparison of two vectors answers a mask by the typing rule `binaryResult` states, so
         * each of the six is the `binary` every other comparison is and nothing here says otherwise.
         * `select` is the branch-free choice `inst.def`'s `Select` row is, with a condition per lane
         * instead of one for the whole value.
         *
         * The member order is `known.def`'s and `lib/Core/Vector.yana`'s: the comparisons in the
         * order the language writes them, and `select` last.
         */
        case Provided::lanewise:
        case Provided::lanewiseScalar:
            switch(member) {
                case 0: return binary(BinaryOp::Eq);
                case 1: return binary(BinaryOp::Ne);
                case 2: return binary(BinaryOp::Lt);
                case 3: return binary(BinaryOp::Le);
                case 4: return binary(BinaryOp::Gt);
                case 5: return binary(BinaryOp::Ge);
                case 6:
                    assertTrue(count == 3);
                    return builder.addSelect(source, *arguments[0], *arguments[1], *arguments[2]);
            }
            break;

        case Provided::Count:
            break;
    }

    // A member `provided.def`'s class row says exists and no arm above answers - a mismatch between
    // this file and `known.def`'s member order, which is a mistake in the compiler.
    assertTrue(false);
    return nullptr;
}

} // namespace mir
