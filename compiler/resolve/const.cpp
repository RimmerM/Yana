#include "const.h"
#include "name.h"
#include "../parse/ast.h"

/*
 * The record behind an enumeration type, or null for anything else.
 *
 * A record whose constructors all carry nothing is its discriminant and nothing else, so a value of
 * one is a number in exactly the way an integer is - which is what makes `False` as much a constant
 * as `0`, and what a flags type is made of.
 */
static RecordType* enumRecord(GlobalBase base, TypePtr type) {
    if(!type) return nullptr;

    auto value = base[type];
    if(value->kind != Type::Record) return nullptr;

    auto record = (RecordType*)value;
    return record->layout == RecordType::Enum ? record : nullptr;
}

// Whether a value of this type can be written as a constant at all.
static bool hasConstantForm(GlobalBase base, TypePtr type) {
    return isInteger(base, type) || isPointer(base, type) || isFloat(base, type) ||
           enumRecord(base, type) != nullptr;
}

/*
 * A number as it was written: the sign and the magnitude, kept apart.
 *
 * This is the shape the *source* has - the lexer produces only the magnitude, and a `-` in front of
 * it is a different token - and it is kept until the type has been chosen because every question
 * asked of the number needs the type to be right. Negating into 64-bit two's complement up front
 * answers three of them wrongly: `-1 :: Float` becomes the `F64` of `0xFFFF...FFFF`, which is 1.8e19
 * rather than -1; `18446744073709551615 :: I64` becomes the same bits as `-1` and is accepted as it,
 * which is the one thing a range check on an `I64` is for; and negating `I64`'s own minimum is
 * signed overflow, so the input that most needs to work is the one that is undefined.
 *
 * `source` is the whole constant, sign included, so a diagnostic underlines what was written rather
 * than the digits after the minus.
 */
struct WrittenNumber {
    const ast::Literal* literal = nullptr;
    ast::Literal::Kind kind = ast::Literal::Int;
    LocationId source = kNullLocation;
    bool negative = false;
};

/*
 * The literal an expression is, with a leading `-` peeled off rather than folded in.
 *
 * `parseLeftPattern` does fold one, and the two are deliberately not one function: a pattern has no
 * expected type at the point it is parsed, so the sign has nowhere to wait, and a constant does -
 * which is the whole of what the paragraph above is about. What they share is the shape they look
 * for, which is three lines.
 *
 * Only over a literal, and only for `-`. Anything else that reduces to a number is arithmetic, and a
 * declaration that evaluated arithmetic would be a constant folder with the language's whole
 * operator set behind it - which is a feature, and a different one.
 */
static Maybe<WrittenNumber> writtenNumber(Module& module, const ast::Expr& expr) {
    auto value = &expr;
    auto negative = false;

    if(expr.kind == ast::Expr::Prefix) {
        auto& prefix = *module.parse[expr.prefix];
        if(prefix.op.kind != ast::Expr::Var || prefix.op.var != Context::nameHash("-"_v)) return Nothing();

        value = &prefix.on;
        negative = true;
    }

    if(!ast::isLiteral(*value)) return Nothing();

    auto kind = ast::Literal::Kind(value->kind - ast::Expr::Lit);

    // A sign belongs to a number and to nothing else, so `-"a"` is not a written string but an
    // operator applied to one - which is arithmetic, and refused as such.
    if(negative && kind != ast::Literal::Int && kind != ast::Literal::Float && kind != ast::Literal::Double) {
        return Nothing();
    }

    return Just(WrittenNumber { &value->lit, kind, expr.source, negative });
}

// Whether a written literal is a constant of a given type, and if not, which of the two ways it is
// not one - the type has no constant of that kind at all, or it has and this is not one of them.
enum class LiteralFit: U8 {
    Ok,
    Kind,
    Range,
};

/*
 * A written number, reduced to the bits a value of `type` holds.
 *
 * The one place that question is answered. It used to be answered twice and the two answers
 * differed, which is the divergence const.h's header comment describes.
 */
static LiteralFit literalBitsAt(Module& module, const WrittenNumber& written, TypePtr type, U64& bits) {
    auto global = *module.types;
    auto& literal = *written.literal;

    if(written.kind == ast::Literal::Int) {
        /*
         * A pointer's constant is an address written as an integer, which is how a null pointer is
         * spelled in both positions. How wide one is is the target's answer and not known here, so
         * the only thing checked is the sign: an address counts up from zero, and a negative one is
         * a number that means nothing rather than a large one.
         */
        if(isPointer(global, type)) {
            if(written.negative && literal.i() != 0) return LiteralFit::Range;
            bits = literal.i();
            return LiteralFit::Ok;
        }

        if(isInteger(global, type)) {
            auto& integer = *(IntType*)global[type];
            if(!integerHolds(integer, literal.i(), written.negative)) return LiteralFit::Range;

            // Negated in the width the value is held at, and then reduced to the type's normal form -
            // the same form `makeInt` puts every other integer constant in, so a global's `initial`
            // and the `ConstInt` a read of it folds to are the same bits.
            auto magnitude = literal.i();
            bits = reduceToWidth(integer, written.negative ? U64(0) - magnitude : magnitude);
            return LiteralFit::Ok;
        }
    }

    if(isFloat(global, type) &&
       (written.kind == ast::Literal::Int || written.kind == ast::Literal::Float ||
        written.kind == ast::Literal::Double)) {
        auto number = written.kind == ast::Literal::Int   ? F64(literal.i())
                    : written.kind == ast::Literal::Float ? F64(literal.f)
                                                          : literal.d();

        // The sign is applied to the *number*, which is what it was written on. A float's sign is a
        // bit of its own, so this is exact for every magnitude and is where `-0.0` comes from.
        if(written.negative) number = -number;

        // Written as the bits the storage will occupy, so that nothing has to convert again later.
        bits = floatBits(global, type, number);
        return LiteralFit::Ok;
    }

    /*
     * A boolean literal, which no source program produces - `True` and `False` are nullary
     * constructors and reach the constructor form instead. It is handled anyway because the
     * expression resolver produces the same value for one, and a constant that disagreed with what
     * an expression means would be the defect this file exists to remove.
     */
    if(written.kind == ast::Literal::Bool && type == module.scalar.bool_) {
        bits = literal.b ? 1 : 0;
        return LiteralFit::Ok;
    }

    return LiteralFit::Kind;
}

/*
 * The type a literal takes where the position did not say - a global written without `:: T`.
 *
 * Asked of the literal's *class* rather than written down here, because "an integer literal is an
 * `Int` unless something says otherwise" is a fact about `default FromInt = Int` in Core rather than
 * about this file: a program that moved the default and found a global still starting at an `Int`
 * would have two answers to one question. This is `literalDefault` for the one-class case, which is
 * every case a constant has - a literal collects a second class only by meeting another literal, and
 * there is nothing here for it to meet.
 *
 * Null for a character literal, which has neither a class nor a type in the language yet.
 */
static TypePtr literalDefaultType(Module& module, ast::Literal::Kind literal) {
    auto global = *module.types;

    auto classDefault = [&](GlobalPtr<TypeClass> typeClass) -> TypePtr {
        return typeClass ? global[typeClass]->defaultType : nullptr;
    };

    switch(literal) {
        case ast::Literal::Int:
            return classDefault(module.coreClasses.fromInt);
        case ast::Literal::Float:
        case ast::Literal::Double:
            return classDefault(module.coreClasses.fromDecimal);

        // Neither of these is class-dispatched, which is why they are the two literal kinds with no
        // `From…` class: a boolean literal is the enumeration `Bool`, and a string literal a `String`.
        case ast::Literal::Bool:
            return module.scalar.bool_;
        case ast::Literal::String:
            return module.scalar.string_;
        default:
            return nullptr;
    }
}

/*
 * The failure a caller asked to be told about rather than to have reported - see the `notConstant`
 * parameter of `evaluateConstant`.
 *
 * Only the outcomes that mean "this is not a constant *form*" go through here. The rest - a literal
 * out of range for its type, an ascription that disagrees with the position, a constructor of the
 * wrong record - are the right form with the wrong contents, and there is no other reading of them
 * for a caller to prefer.
 */
static Constant unreportedNonConstant(bool* notConstant) {
    *notConstant = true;
    return {};
}

/*
 * A constructor written where a constant is wanted.
 *
 * Only a nullary constructor of an enumeration is one, and each of the other four outcomes is
 * reported as what it is rather than as "not a literal": a name that resolves to nothing, a
 * constructor that carries a value, one whose record has payloads elsewhere, and one that produces
 * the wrong type. That is most of what centralizing this bought - a global rejected every one of
 * them with a message about literals, because it had never heard of the form.
 */
static Constant constructorConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what,
                                    bool* notConstant) {
    auto& context = module.context;
    auto& diagnostics = context.diagnostics;
    auto global = *module.types;
    auto& construct = *module.parse[expr.con];

    // `Con` and `Record.Con` are both an `ast::Type::Con`; anything else in this position is an
    // applied or structural type, which names no constructor.
    if(construct.type.kind != ast::Type::Con) {
        if(notConstant) return unreportedNonConstant(notConstant);
        diagnostics.error("%@ must be a literal or a nullary constructor"_v, expr.source, what);
        return {};
    }

    auto name = context.findName(construct.type.name);
    auto found = findConstructor(module, construct.type.name, expr.source);

    if(!found) {
        diagnostics.error("no constructor named %@ is visible here"_v, expr.source, name);
        return {};
    }

    // The declaration, never an instantiation of one - see findConstructor. Its layout and its
    // constructors are the declaration's in either case, which is what makes both questions below
    // answerable before the type is known.
    auto reference = found.unwrap();
    auto declaration = (RecordType*)global[reference.record];
    TypePtr declared = (Type*)declaration - global;

    if(declaration->layout != RecordType::Enum) {
        if(notConstant) return unreportedNonConstant(notConstant);
        diagnostics.error("%@ cannot be %@ - a value of %@ is storage rather than a number, and only a record whose constructors all carry nothing has a constant form"_v,
                          expr.source, what, name, describeType(context, global, declared));
        return {};
    }

    // An enumeration's constructor carries nothing by construction, so an argument list here is a
    // call that cannot exist rather than a constant that is too complicated.
    if(construct.args.isNotEmpty()) {
        diagnostics.error("nullary constructor does not take arguments"_v, expr.source);
        return {};
    }

    /*
     * Which type the constructor produces, which is `constructedType`'s question with its middle
     * arm removed.
     *
     * That function has three: a declaration that is not generic produces itself, an expected type
     * built from the same declaration is taken as written, and anything else is solved for from what
     * the constructor was handed. The third cannot apply here - a nullary constructor is handed
     * nothing, so there is nothing for a solve to read - which is why this is a specialization of
     * that rule rather than a second one, and why the failure it ends at borrows that function's
     * words. `data State(a) = Off | On` is the type it is about: `On :: State(Int)` is a constant,
     * and `On` on its own is a global whose type nothing decided.
     */
    auto env = declaration->gen ? global[declaration->gen] : nullptr;
    auto generic = env && env->types.isNotEmpty();
    auto instance = expected && global[expected]->kind == Type::Record ? (RecordType*)global[expected] : nullptr;

    TypePtr type = declared;

    if(generic) {
        if(!instance || instance->base(global) != reference.record) {
            diagnostics.error("cannot infer the type arguments of %@ here - give the expected type"_v,
                              expr.source, context.findName(declaration->name));
            return {};
        }

        type = expected;
    }

    if(expected && !sameType(type, expected)) {
        diagnostics.error("constructor produces %@ but %@ is expected"_v, expr.source,
                          describeType(context, global, type), describeType(context, global, expected));
        return {};
    }

    return Constant { type, reference.index };
}

Constant evaluateConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what,
                          bool* notConstant) {
    auto& context = module.context;
    auto& diagnostics = context.diagnostics;
    auto global = *module.types;
    auto value = &expr;

    /*
     * `0 :: %U8` - the ascription, which is how a position with no type of its own says what the
     * constant is, and the only way a global says what it is at all: a `let` pattern carries no
     * type annotation.
     *
     * A position that already has a type still accepts one, and they then have to agree. Taking the
     * written one on its word is precisely the mistake that made `let &slot = 0 :: Pair` compile.
     */
    if(value->kind == ast::Expr::Coerce) {
        auto& coerce = *module.parse[value->coerce];
        auto written = resolveType(module, coerce.type);

        // The written type failed to resolve, which `resolveType` has already reported - as the
        // error type rather than as nothing, which is what every other reader of it tests for. A
        // second diagnostic about the constant form of a type that was never named would be about
        // this file's own recovery and not about anything the author wrote.
        if(!written || global[written]->kind == Type::Error) return {};

        if(expected && !sameType(written, expected)) {
            diagnostics.error("%@ is written `:: %@` but the position it is in has type %@"_v,
                              coerce.type.source, what, describeType(context, global, written),
                              describeType(context, global, expected));
            return {};
        }

        expected = written;
        value = &coerce.target;
    }

    if(value->kind == ast::Expr::Con) return constructorConstant(module, *value, expected, what, notConstant);

    // A number as written, which is the literal and whether a `-` was in front of it.
    auto number = writtenNumber(module, *value);

    if(!number) {
        if(notConstant) return unreportedNonConstant(notConstant);
        diagnostics.error("%@ must be a literal or a nullary constructor, optionally written `constant :: Type` - there is no program point at which a declaration's own code would run"_v,
                          value->source, what);
        return {};
    }

    auto& written = number.unwrap();
    auto type = expected ? expected : literalDefaultType(module, written.kind);

    // A character literal, or a class whose default was taken away. The expression resolver says the
    // second half of this in `materializeLiteral`, and for the same reason.
    if(!type) {
        diagnostics.error("nothing decides the type of %@ - write `constant :: Type`"_v, written.source, what);
        return {};
    }

    /*
     * Two different mistakes, and telling them apart is the whole use of these messages: a type with
     * no constant form at all, against a literal that is not one of the constants the type does
     * have. The first is what a global of a memory type is - there is no spelling for a constant of
     * one, so the honest answer is that it cannot have one yet rather than that it starts empty.
     */
    if(!hasConstantForm(global, type)) {
        if(notConstant) return unreportedNonConstant(notConstant);
        diagnostics.error("%@ has type %@, which has no constant form - only an integer, pointer, floating-point or enumeration type has one"_v,
                          written.source, what, describeType(context, global, type));
        return {};
    }

    U64 bits = 0;

    switch(literalBitsAt(module, written, type, bits)) {
        case LiteralFit::Ok:
            return Constant { type, bits };
        case LiteralFit::Range:
            diagnostics.error("%@ is out of range for type %@ - a declaration takes the value it is written as, and this is not one of them"_v,
                              written.source, what, describeType(context, global, type));
            return {};
        case LiteralFit::Kind:
            diagnostics.error("%@ is not a constant of type %@ - a declaration takes the value it is written as, since there is no conversion it could run"_v,
                              written.source, what, describeType(context, global, type));
            return {};
    }

    return {};
}
