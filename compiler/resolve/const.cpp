#include "const.h"
#include "name.h"
#include "../parse/ast.h"

TypePtr constantType(ModuleBase local, ConstantPtr constant) {
    return constant ? local[constant]->type : nullptr;
}

// The fields of a tuple, checked for the indirection that has no constant form - see the header.
static bool tupleHasStaticForm(GlobalBase global, ModuleBase local, TypePtr tuple,
                               ModuleList<ConstantPtr, false>& children);

bool constantHasStaticForm(GlobalBase global, ModuleBase local, ConstantPtr constant) {
    if(!constant) return true;

    auto& value = *local[constant];

    switch(value.kind) {
        case ConstKind::Scalar:
        case ConstKind::Address:
            return true;

        case ConstKind::String:
            // The text alone, which is what a position that does not lay the constant out produces.
            // A global always asks for the static form, so one that got only text is a target this
            // has nothing to check - see stringConstant.
            for(auto child: value.children.contents(local)) {
                if(!constantHasStaticForm(global, local, child)) return false;
            }

            return true;

        case ConstKind::Aggregate: {
            auto declared = global[value.type];

            // A fixed array's elements are its own storage, so only what they hold can decline.
            if(declared->kind == Type::Array || declared->kind == Type::String) {
                for(auto child: value.children.contents(local)) {
                    if(!constantHasStaticForm(global, local, child)) return false;
                }

                return true;
            }

            return tupleHasStaticForm(global, local, value.type, value.children);
        }

        case ConstKind::Construct: {
            auto declared = global[value.type];
            if(declared->kind != Type::Record) return true;

            auto& record = *(RecordType*)declared;
            if(value.index >= record.constructors.size()) return true;

            auto constructor = record.constructors.get(global, U16(value.index));
            if(constructor.boxed) return false;

            auto content = constructor.content;
            if(content && global[content]->kind == Type::Tup) {
                return tupleHasStaticForm(global, local, content, value.children);
            }

            for(auto child: value.children.contents(local)) {
                if(!constantHasStaticForm(global, local, child)) return false;
            }

            return true;
        }
    }

    return true;
}

static bool tupleHasStaticForm(GlobalBase global, ModuleBase local, TypePtr tuple,
                               ModuleList<ConstantPtr, false>& children) {
    auto& fields = ((TupType*)global[tuple])->fields;
    auto items = children.contents(local);

    for(Size i = 0; i < items.size(); i++) {
        if(i < fields.size() && fields.get(global, i).boxed) return false;
        if(!constantHasStaticForm(global, local, items[i])) return false;
    }

    return true;
}

ConstantPtr fieldDefaultOf(GlobalBase global, GlobalList<FieldDefault>* defaults, U16 field) {
    if(!defaults) return nullptr;

    for(auto def: defaults->contents(global)) {
        if(def.field == field) return def.value;
    }

    return nullptr;
}

// One node, in the IR region - see ConstValue.
static ConstValue* makeConst(Module& module, TypePtr type, ConstKind kind) {
    return new (module.arena) ConstValue(type, kind);
}

static ConstantPtr scalarConstant(Module& module, TypePtr type, U64 bits) {
    auto value = makeConst(module, type, ConstKind::Scalar);
    value->bits = bits;
    return value - *module.arena;
}

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

/*
 * Whether a *written number* can be a constant of this type at all.
 *
 * Deliberately narrower than "does this type have a constant form": a record has one now, and it is
 * a construction rather than a number. What this decides is the message a bare literal at a type
 * gets, which is the case where the two forms have been confused - `let &slot = 0 :: Pair`.
 */
static bool hasNumericForm(GlobalBase base, TypePtr type) {
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
static ConstantPtr unreportedNonConstant(bool* notConstant) {
    *notConstant = true;
    return nullptr;
}

/*
 * The tuple whose fields a constructor's arguments fill, or null where its payload is carried whole.
 *
 * `Just(a)` and `Point {x: Int, y: Int}` are the two shapes, and the difference is exactly this: a
 * constructor wrapping one unnamed type has one argument and no fields to name.
 */
static TupType* contentFields(GlobalBase global, TypePtr content) {
    if(!content || global[content]->kind != Type::Tup) return nullptr;
    return (TupType*)global[content];
}

/*
 * One value per field of `tuple`, matching arguments to fields the way `fillTuple` does - by name
 * where they have one, by position otherwise - and falling back to each field's declared default.
 *
 * Deliberately the same rules and the same messages as the expression form, because a constructed
 * value should not depend on whether it is a constant: a field written twice, a name that is not a
 * field and a field with neither an argument nor a default are mistakes wherever they appear.
 *
 * What differs is only the answer: this produces constants, so a field whose argument is not one is
 * this whole construction's `notConstant` rather than an error of its own.
 */
static bool fillTupleConstant(Module& module, TupType& tuple, ast::ParseList<ast::TupArg> astArgs,
                              GlobalList<FieldDefault>* defaults, StringView what, LocationId source,
                              ModuleList<ConstantPtr, false>& into, bool staticForm, bool* notConstant);

/*
 * A constructor written where a constant is wanted.
 *
 * Four shapes reach here and all four are constants now: a nullary constructor of an enumeration,
 * which is a number; a record construction `Point {x: 1, y: 2}`; a constructor carrying a payload,
 * `Just(5)`; and any nesting of those. What is *not* one is a generic declaration whose arguments
 * nothing decides - the solve `constructedType` runs needs resolved argument values, and a constant
 * has none - so the instance has to be named, exactly as it does for a nullary constructor.
 */
static ConstantPtr constructorConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what,
                                       bool staticForm, bool* notConstant) {
    auto& context = module.context;
    auto& diagnostics = context.diagnostics;
    auto global = *module.types;
    auto& construct = *module.parse[expr.con];

    // `Con` and `Record.Con` are both an `ast::Type::Con`; anything else in this position is an
    // applied or structural type, which names no constructor.
    if(construct.type.kind != ast::Type::Con) {
        if(notConstant) return unreportedNonConstant(notConstant);
        diagnostics.error("%@ must be a literal or a constructor"_v, expr.source, what);
        return nullptr;
    }

    auto name = context.findName(construct.type.name);
    auto found = findConstructor(module, construct.type.name, expr.source);

    if(!found) {
        diagnostics.error("no constructor named %@ is visible here"_v, expr.source, name);
        return nullptr;
    }

    // The declaration, never an instantiation of one - see findConstructor. Which constructors it
    // has and what each of them carries is the declaration's in either case, so the shape below can
    // be read off it before the instance is decided.
    auto reference = found.unwrap();
    auto declaration = (RecordType*)global[reference.record];
    TypePtr declared = (Type*)declaration - global;

    /*
     * Which type the constructor produces, which is `constructedType`'s question with its middle
     * arm removed.
     *
     * That function has three: a declaration that is not generic produces itself, an expected type
     * built from the same declaration is taken as written, and anything else is solved for from the
     * *resolved values* the constructor was handed. The third cannot apply here - a constant is not
     * a resolved value and there is no solver in this file - which is why this is a specialization
     * of that rule rather than a second one, and why the failure it ends at borrows that function's
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
            return nullptr;
        }

        type = expected;
    }

    if(expected && !sameType(type, expected)) {
        diagnostics.error("constructor produces %@ but %@ is expected"_v, expr.source,
                          describeType(context, global, type), describeType(context, global, expected));
        return nullptr;
    }

    // An enumeration is its discriminant and nothing else, so constructing one produces the index
    // rather than storage holding it - which is the same value `resolveConstruct` produces, and the
    // reason this was the one constructor form a constant had before there were any others.
    auto record = (RecordType*)global[type];
    if(record->layout == RecordType::Enum) {
        if(construct.args.isNotEmpty()) {
            diagnostics.error("nullary constructor does not take arguments"_v, expr.source);
            return nullptr;
        }

        // The pinned number where there is one, exactly as `resolveConstruct` produces it - the two
        // have to agree, since a global's initializer and an expression are the same construction.
        return scalarConstant(module, type, U64(record->constructors.get(global, reference.index).value));
    }

    auto value = makeConst(module, type, ConstKind::Construct);
    value->index = reference.index;

    auto content = record->constructors.get(global, reference.index).content;
    auto args = construct.args.contents(module.parse);

    // Nothing to carry, which is a constructor of a sum whose payload is elsewhere - `Nothing` of a
    // `Maybe(Int)`. The tag is the whole of the value, and `children` stays empty.
    if(!content || isUnit(global, content)) {
        if(args.size()) {
            diagnostics.error("nullary constructor does not take arguments"_v, expr.source);
            return nullptr;
        }

        return value - *module.arena;
    }

    if(auto tuple = contentFields(global, content)) {
        /*
         * Defaults are read from the *declaration* rather than from `record`, which may be an
         * instantiation of it: what a field falls back to is a property of the declaration, and an
         * instantiation can be made before the declaration's defaults have been read. This is
         * `resolveConstruct`'s own comment, and `reference.record` is always the declaration.
         */
        auto declared_ = declaration->constructors.get(global, reference.index);
        if(!fillTupleConstant(module, *tuple, construct.args, &declared_.defaults, what, expr.source,
                              value->children, staticForm, notConstant)) {
            return nullptr;
        }

        return value - *module.arena;
    }

    // A payload carried whole, which takes exactly one positional argument - `resolveConstruct`'s
    // last arm, and the same message.
    if(args.size() != 1 || args[0].name) {
        diagnostics.error("constructor requires one positional argument"_v, expr.source);
        return nullptr;
    }

    auto payload = evaluateConstant(module, args[0].value, content, what, staticForm, notConstant);
    if(!payload) return nullptr;

    value->children.push(module.arena, payload);
    return value - *module.arena;
}

static bool fillTupleConstant(Module& module, TupType& tuple, ast::ParseList<ast::TupArg> astArgs,
                              GlobalList<FieldDefault>* defaults, StringView what, LocationId source,
                              ModuleList<ConstantPtr, false>& into, bool staticForm, bool* notConstant) {
    auto& context = module.context;
    auto& diagnostics = context.diagnostics;
    auto global = *module.types;
    auto args = astArgs.contents(module.parse);

    SmallArray<ConstantPtr, 8> values;
    SmallArray<bool, 8> written;
    for(Size i = 0; i < tuple.fields.size(); i++) {
        values.push(nullptr);
        written.push(false);
    }

    Size positional = 0;

    for(auto arg: args) {
        Size index = maxLimit<Size>;

        if(arg.name) {
            for(Size i = 0; i < tuple.fields.size(); i++) {
                if(tuple.fields.get(global, i).name == arg.name) {
                    index = i;
                    break;
                }
            }
        } else {
            while(positional < written.size() && written[positional]) positional++;
            if(positional < written.size()) index = positional++;
        }

        if(index == maxLimit<Size>) {
            diagnostics.error(arg.name ? "constructed tuple has no field with this name"_v
                                       : "too many tuple arguments"_v, arg.value.source);
            return false;
        }

        if(written[index]) {
            diagnostics.error("tuple field specified more than once"_v, arg.value.source);
            return false;
        }

        written[index] = true;

        auto expected = tuple.fields.get(global, index).type;
        values[index] = evaluateConstant(module, arg.value, expected, what, staticForm, notConstant);

        // A unit field is a constant that occupies nothing, so it has no node and is not a failure -
        // the same silence `write` keeps for a unit place.
        if(!values[index] && !isUnit(global, expected)) return false;
    }

    for(Size i = 0; i < values.size(); i++) {
        if(written[i]) continue;

        auto field = tuple.fields.get(global, i);

        if(auto def = fieldDefaultOf(global, defaults, U16(i))) {
            values[i] = def;
        } else if(isUnit(global, field.type)) {
            continue;
        } else if(field.name) {
            diagnostics.error("no value provided for field %@"_v, source, context.findName(field.name));
            return false;
        } else {
            diagnostics.error("no value provided for tuple field"_v, source);
            return false;
        }
    }

    for(auto value: values) into.push(module.arena, value);
    return true;
}

/*
 * A tuple literal - `(1, 2)`, and `{x: 1, y: 2}` where the position's type is an anonymous tuple.
 *
 * The position has to say what it is, for the reason a tuple literal always needs one: `(1, 2)` on
 * its own names no field types and no field names, and there is nothing here to infer them from.
 */
static ConstantPtr tupleConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what,
                                 bool staticForm, bool* notConstant) {
    auto& context = module.context;
    auto global = *module.types;
    auto tuple = expected ? contentFields(global, expected) : nullptr;

    if(!tuple) {
        if(notConstant) return unreportedNonConstant(notConstant);

        if(expected) {
            context.diagnostics.error("%@ is written as a tuple, but the position it is in has type %@"_v,
                                      expr.source, what, describeType(context, global, expected));
        } else {
            context.diagnostics.error("nothing decides the type of %@ - a tuple written out names no field types of its own, so write `constant :: Type`"_v,
                                      expr.source, what);
        }

        return nullptr;
    }

    auto args = expr.tup;
    auto value = makeConst(module, expected, ConstKind::Aggregate);
    if(!fillTupleConstant(module, *tuple, args, nullptr, what, expr.source, value->children, staticForm, notConstant)) {
        return nullptr;
    }

    return value - *module.arena;
}

/*
 * An array literal at a `[T *n]` - Implementation-Containers.md §8's fixed array, which is the one
 * of the two shapes `[1, 2, 3]` builds that has a constant form.
 *
 * The other is `Array(a)`, whose run is an allocation: what makes a fixed array constant is that its
 * elements are the value's own bytes, so there is nothing to allocate and nothing to hand back. A
 * growable array written here is therefore not a constant *form* rather than a bad constant, which
 * is what lets a root module keep initializing one at startup.
 *
 * The length is a check rather than an inference, exactly as it is for the expression form: `n` is
 * in the type and the literal's length is syntax, so the two are compared.
 */
static ConstantPtr arrayConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what,
                                 bool staticForm, bool* notConstant) {
    auto& context = module.context;
    auto global = *module.types;

    if(!expected || global[expected]->kind != Type::Array) {
        if(notConstant) return unreportedNonConstant(notConstant);

        if(expected) {
            context.diagnostics.error("%@ is written as an array literal, but %@ has no constant form - only a fixed array `[T *n]` does, since its elements are the value's own storage rather than a run it allocates"_v,
                                      expr.source, what, describeType(context, global, expected));
        } else {
            context.diagnostics.error("nothing decides the type of %@ - an array literal is a fixed array only where the position says so, so write `constant :: [T *n]`"_v,
                                      expr.source, what);
        }

        return nullptr;
    }

    auto& array = *(ArrayType*)global[expected];
    auto items = expr.arr;
    auto elements = items.contents(module.parse);

    // A constant is only ever evaluated against a concrete expected type, so the count is a number
    // here - a generic `[T *n]` has no constant to be.
    auto length = constValue(global, array.count);

    if(elements.size() != length) {
        context.diagnostics.error("%@ has %@ elements, but %@ holds %@"_v, expr.source, what,
                                  elements.size(), describeType(context, global, expected), length);
        return nullptr;
    }

    auto value = makeConst(module, expected, ConstKind::Aggregate);

    for(auto element: elements) {
        auto content = evaluateConstant(module, element, array.content, what, staticForm, notConstant);
        if(!content && !isUnit(global, array.content)) return nullptr;

        value->children.push(module.arena, content);
    }

    return value - *module.arena;
}

/*
 * The single constructor's fields of a record, or null where the type is not that shape.
 *
 * Only used to walk the containers a native string literal is made of, which are all single-
 * constructor records - see `nativeStringConstant`, and `nativeField` below, which is how a leaf of
 * one is named.
 */
static TupType* recordFields(GlobalBase global, TypePtr type) {
    if(!type || global[type]->kind != Type::Record) return nullptr;

    auto& record = *(RecordType*)global[type];
    if(record.layout != RecordType::Single || record.constructors.size() != 1) return nullptr;

    return contentFields(global, record.constructors.get(global, 0).content);
}

/*
 * A native string literal, as the value `stringLiteral` builds - Implementation-String.md part 9,
 * written as a constant rather than as a call.
 *
 * The two are deliberately the same value:
 *
 *     stringFromData(StringData {bytes: Array {run: Run {items: bytes, capacity: length,
 *                                                        ownsHeap: runBorrowed}, length: length}})
 *
 * and every leaf of it is a number or an address, which is exactly what a constant is made of. The
 * bytes go into the module's data as an ordinary global exactly as the call form's do, and the run
 * borrows them - so a constant string owns nothing, is dropped by doing nothing, and grows by the
 * copy-on-write path `runBorrowed` already means.
 *
 * The record is walked by *name* rather than by position, and a shape that does not match is an
 * internal error rather than a wrong constant: this file is naming another module's declarations, and
 * the honest failure for that is to say the declaration changed under it.
 */
static ConstantPtr nativeStringConstant(Module& module, const ast::Expr& expr, StringId text, StringView what) {
    auto& context = module.context;
    auto global = *module.types;

    // Which field of `record` is called `name`, as the type it holds and the index it sits at.
    struct Field { TypePtr type = nullptr; U16 index = 0; bool found = false; };
    auto field = [&](TypePtr record, const char* name, Size length) -> Field {
        auto tuple = recordFields(global, record);
        if(!tuple) return {};

        auto wanted = context.addUnqualifiedName(name, length);
        for(Size i = 0; i < tuple->fields.size(); i++) {
            auto entry = tuple->fields.get(global, i);
            if(entry.name == wanted) return Field { entry.type, U16(i), true };
        }

        return {};
    };

    auto content = module.scalar.stringContent;
    auto bytes = field(content, "bytes", 5);
    auto run = field(bytes.type, "run", 3);
    auto length = field(bytes.type, "length", 6);
    auto items = field(run.type, "items", 5);
    auto capacity = field(run.type, "capacity", 8);
    auto ownsHeap = field(run.type, "ownsHeap", 8);

    if(!bytes.found || !run.found || !length.found || !items.found || !capacity.found || !ownsHeap.found) {
        context.diagnostics.error("internal: %@ is a string, but this target's string layout is not the one this compiler writes"_v,
                                  expr.source, what);
        return nullptr;
    }

    /*
     * The bytes, as a global of their own, named by position rather than by content - the same
     * counter and the same reasoning as `resolveString`, which is the other half of this: a literal
     * in an expression and a literal in a declaration produce the same two things, and only where
     * the value goes differs.
     */
    auto decoded = context.findName(text);
    auto size = decoded.size();

    StringBuilder name;
    name << context.findName(module.name) << ".string$";
    name.appendValue(module.stringLiteralCount++);

    auto blob = module.addGlobal(builtName(context, name), expr.source);
    blob->type = module.scalar.string_;
    blob->literalBytes = ByteBuffer((Byte*)module.arena.alloc(size), size);
    copy((const Byte*)decoded.text(), blob->literalBytes.ptr, size);
    blob->used = true;

    // A count at the width the field holds it - `Count` is a `@bits(30) U32`, so this is the same
    // narrowing `length :: Count` performs in the call form.
    auto count = [&](TypePtr type) -> ConstantPtr {
        auto integer = isInteger(global, type) ? (IntType*)global[type] : nullptr;
        return scalarConstant(module, type, integer ? reduceToWidth(*integer, U64(size)) : U64(size));
    };

    auto address = makeConst(module, items.type, ConstKind::Address);
    address->global = blob - *module.arena;

    // Each of the three is a single-constructor record, so each is that constructor - the same node
    // `Point {x: 1, y: 2}` produces, since a record has exactly one constant form however many
    // constructors it declares.
    auto runValue = makeConst(module, run.type, ConstKind::Construct);
    runValue->children.push(module.arena, address - *module.arena);
    runValue->children.push(module.arena, count(capacity.type));
    runValue->children.push(module.arena, scalarConstant(module, ownsHeap.type, 0));

    auto arrayValue = makeConst(module, bytes.type, ConstKind::Construct);
    arrayValue->children.push(module.arena, runValue - *module.arena);
    arrayValue->children.push(module.arena, count(length.type));

    auto data = makeConst(module, content, ConstKind::Construct);
    data->children.push(module.arena, arrayValue - *module.arena);

    // The literal, with the record that describes its bytes underneath - which is what
    // `computeString` says a native string's representation is. The text stays on the node beside
    // it, because the *value* form of this constant is not these bytes at all: a field default
    // reaching `resolveString` builds the same string the way any other expression would.
    auto string = makeConst(module, module.scalar.string_, ConstKind::String);
    string->text = text;
    string->children.push(module.arena, data - *module.arena);
    return string - *module.arena;
}

/*
 * A string literal.
 *
 * The two targets diverge completely, which is `resolveString`'s split and not a second one: a host
 * string is one value that only a source literal produces, so it stays text here and becomes text
 * there; a native string is two words describing bytes, so it is built out of them.
 */
static ConstantPtr stringConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what,
                                  bool staticForm) {
    auto& context = module.context;
    auto global = *module.types;

    if(expected && global[expected]->kind != Type::String) {
        context.diagnostics.error("%@ is written as a string, but the position it is in has type %@"_v,
                                  expr.source, what, describeType(context, global, expected));
        return nullptr;
    }

    /*
     * The text alone, which is the whole node in two of the three positions and on one of the two
     * targets. A host string is one value with nothing underneath it; and a field default or a
     * default argument is *built* wherever it is left out, through `resolveString`, so a static form
     * for one would be a blob global nothing ever emits.
     */
    if(!staticForm || isJsMode(context.settings.mode) || !module.scalar.stringContent) {
        if(staticForm && !isJsMode(context.settings.mode) && !module.scalar.stringContent) {
            context.diagnostics.error("internal: no string layout for this target"_v, expr.source);
            return nullptr;
        }

        auto value = makeConst(module, module.scalar.string_, ConstKind::String);
        value->text = expr.lit.s;
        return value - *module.arena;
    }

    return nativeStringConstant(module, expr, expr.lit.s, what);
}

ConstantPtr evaluateConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what,
                             bool staticForm, bool* notConstant) {
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
        if(!written || global[written]->kind == Type::Error) return nullptr;

        if(expected && !sameType(written, expected)) {
            diagnostics.error("%@ is written `:: %@` but the position it is in has type %@"_v,
                              coerce.type.source, what, describeType(context, global, written),
                              describeType(context, global, expected));
            return nullptr;
        }

        expected = written;
        value = &coerce.target;
    }

    /*
     * The forms that build a value rather than name one, each dispatched on what was *written*
     * rather than on the type the position has. That is the same order the expression resolver uses,
     * and it is what keeps the diagnostics about the mistake: `[1, 2] :: Point` is an array literal
     * at the wrong type, not a `Point` written strangely.
     */
    switch(value->kind) {
        case ast::Expr::Con:
            return constructorConstant(module, *value, expected, what, staticForm, notConstant);
        case ast::Expr::Tup:
            return tupleConstant(module, *value, expected, what, staticForm, notConstant);
        case ast::Expr::Array:
            return arrayConstant(module, *value, expected, what, staticForm, notConstant);
        default:
            break;
    }

    // A string, which is a literal kind rather than an expression kind - `Expr::Lit` is the base of
    // a range and not a case, which is why this is not one of the arms above.
    if(ast::isLiteral(*value) && ast::Literal::Kind(value->kind - ast::Expr::Lit) == ast::Literal::String) {
        return stringConstant(module, *value, expected, what, staticForm);
    }

    // A number as written, which is the literal and whether a `-` was in front of it.
    auto number = writtenNumber(module, *value);

    if(!number) {
        if(notConstant) return unreportedNonConstant(notConstant);
        diagnostics.error("%@ must be a literal, a constructor or an aggregate of those, optionally written `constant :: Type` - there is no program point at which a declaration's own code would run"_v,
                          value->source, what);
        return nullptr;
    }

    auto& written = number.unwrap();
    auto type = expected ? expected : literalDefaultType(module, written.kind);

    // A character literal, or a class whose default was taken away. The expression resolver says the
    // second half of this in `materializeLiteral`, and for the same reason.
    if(!type) {
        diagnostics.error("nothing decides the type of %@ - write `constant :: Type`"_v, written.source, what);
        return nullptr;
    }

    /*
     * Two different mistakes, and telling them apart is the whole use of these messages: a type no
     * *number* is a value of, against a literal that is not one of the numbers the type does hold.
     * The first is what `let &slot = 0 :: Pair` is - a record is written as a construction, and a
     * zero is not one of its values however many bytes it would have filled.
     */
    if(!hasNumericForm(global, type)) {
        if(notConstant) return unreportedNonConstant(notConstant);
        diagnostics.error("%@ has type %@, which no number is a value of - a record, a tuple or a fixed array is written out, and only an integer, pointer, floating-point or enumeration type is a literal"_v,
                          written.source, what, describeType(context, global, type));
        return nullptr;
    }

    U64 bits = 0;

    switch(literalBitsAt(module, written, type, bits)) {
        case LiteralFit::Ok:
            return scalarConstant(module, type, bits);
        case LiteralFit::Range:
            diagnostics.error("%@ is out of range for type %@ - a declaration takes the value it is written as, and this is not one of them"_v,
                              written.source, what, describeType(context, global, type));
            return nullptr;
        case LiteralFit::Kind:
            diagnostics.error("%@ is not a constant of type %@ - a declaration takes the value it is written as, since there is no conversion it could run"_v,
                              written.source, what, describeType(context, global, type));
            return nullptr;
    }

    return nullptr;
}
