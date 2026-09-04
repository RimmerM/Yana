#include "resolve_internal.h"
#include "name.h"
#include "resolve.h"
#include "verify.h"
#include "summary.h"
#include "analyze.h"
#include "own.h"
#include "../intrinsic/primitive.h"

namespace mir {

void Resolver::unsupported(LocationId source, StringView what) {
    context.diagnostics.error("the ownership resolver does not implement %@ yet"_v, source, what);
}

/*
 * The `@bits(n)` written on a type, or nothing.
 *
 * Every other attribute in a type position is left alone rather than refused, and that is the
 * grammar's doing rather than a decision here: `@name(args)` parses wherever a type does, for
 * features that do not exist yet - `@box`, `@inline`, `@capacity` - and turning "not implemented"
 * into "not allowed" is a refusal each of them would have to undo. A type this stage cannot build at
 * all is reported by the arm that cannot build it.
 */
Maybe<U64> Resolver::readBitsAttribute(ast::Type& type) {
    if(!type.attributes) return Nothing();

    for(auto attribute: parse[type.attributes]->contents(parse)) {
        if(findAttribute(stringView(context.findName(attribute.name))) != DeclAttribute::Bits) continue;

        if(attribute.args.size() != 1) {
            context.diagnostics.error("`@bits` takes one width - `@bits(13) U32`"_v, attribute.source);
            return Nothing();
        }

        // A written number and nothing else. A width is a property of the declaration rather than
        // something a program computes, and an attribute's argument is read before anything in the
        // module can be evaluated - `readValueAttribute` refuses the same shapes for the same reason.
        auto written = attribute.args.get(parse, 0).value;
        if(written.kind != ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
            context.diagnostics.error("`@bits` takes a written number - `@bits(13) U32`"_v,
                                      written.source);
            return Nothing();
        }

        // Read whole rather than narrowed to a width here, so that `@bits(4294967300)` is out of
        // range rather than congruent to four. The range itself is checked against the refined type
        // in `resolveBitsType`, which is the only place that has one: `@bits(40) U32` is out of
        // range and `@bits(40) U64` is not, and the message has to name the type either way.
        return Just(U64(written.lit.i()));
    }

    return Nothing();
}

/*
 * `@bits(n) T` - `doc/spec/repr.md`'s bit-width refinements.
 *
 * A distinct type with a canonical link, which is the whole representation: the *storage* differs, so
 * `sameType` is false and a record holding one lays it out at `n` bits; everything else canonicalizes,
 * so `U64`'s instances serve a `@bits(53) U64` and no width gets a family of its own.
 *
 * Refining an already-refined type re-refines the original rather than nesting, so
 * `@bits(4) @bits(8) U32` is `@bits(4) U32` and there is one canonical form per width.
 */
DeclPtr<Type> Resolver::resolveBitsType(DeclPtr<Type> written, U64 bits, LocationId source) {
    if(!written) return written;

    if(decl()[written]->kind != Type::Int) {
        context.diagnostics.error("@bits can only refine an integer type, and %@ is not one"_v,
                                  source, typeText(context, decl(), written));
        return written;
    }

    auto canonical = canonicalInt(decl(), written);
    auto& original = *(IntType*)decl()[canonical];

    /*
     * A target-answered width cannot be refined, and the reason is that there is nothing to refine it
     * *against*: `bits` on a `Size` is a bound rather than a width, so `@bits(40) Size` would be
     * accepted here and be wider than the word on a target whose word is thirty-two bits - which is
     * the one thing a refinement must never be. Refusing rather than checking against `minBits`
     * because what a refinement buys is a *niche*, and the patterns a narrow value leaves free are a
     * property of the storage it sits in, which is exactly the number this type does not have.
     */
    if(original.isTargetWidth()) {
        context.diagnostics.error("@bits cannot refine %@, whose width is the target's rather than the language's - refine a type that states one"_v,
                                  source, typeText(context, decl(), canonical));
        return canonical;
    }

    // Against the unrefined type's own width rather than the target's, which is the stricter and more
    // useful of the two: `@bits(40) U32` is a mistake on every target, and saying so here means the
    // message names the type the program wrote.
    if(bits == 0 || bits > original.bits) {
        context.diagnostics.error("@bits(%@) is out of range for %@, which holds %@ bits"_v, source,
                                  bits, typeText(context, decl(), canonical), U64(original.bits));
        return canonical;
    }

    // A refinement to the full width is the type itself. Collapsed rather than interned, so that
    // `@bits(32) I32` and `I32` are one type and not two that behave identically.
    if(bits == original.bits) return canonical;

    return program.types.refinedInt(decl(), canonical, U8(bits));
}

/*
 * A written type.
 *
 * The builtin widths, `Bool`, `{}`, `%T` and the records this module declares. A named type that is
 * none of those is a declaration this stage has no table for yet, and saying so by name is more
 * useful than an "unknown type" that could equally mean a typo.
 */
DeclPtr<Type> Resolver::resolveType(ast::Type& type) {
    // Resolved *without* the attribute first, so that the refinement applies to whatever the type
    // turned out to be - an alias, a builtin name, a substituted variable - rather than needing an
    // arm per way of spelling an integer.
    if(auto bits = readBitsAttribute(type)) {
        auto plain = type;
        plain.attributes = nullptr;
        return resolveBitsType(resolveType(plain), bits.unwrap(), type.source);
    }

    switch(type.kind) {
        case ast::Type::Unit:
            return program.types.unit;
        case ast::Type::Con: {
            if(auto builtin = program.types.findBuiltin(context, type.name)) return builtin;

            /*
             * A transparent alias, which answers with what it names - `Alias`.
             *
             * Resolved on demand rather than looked up, because an alias may name one declared below
             * it: reaching this from `resolveAliasTargets`' own walk is what makes `alias A = B` and
             * `alias B = I32` order-independent, and reaching it from an ordinary type position is
             * the same call with the answer already cached.
             */
            if(auto alias = findAlias(context, program, here(), type.name, type.source)) {
                if(isGeneric(decl(), decl()[alias]->gen)) {
                    return applyAlias(alias, nullptr, 0, type.source);
                }

                return aliasTarget(alias);
            }

            if(auto record = findRecord(context, program, here(), type.name, type.source)) {
                // A generic declaration written without arguments. Reported by the one function that
                // knows the arity, rather than by a second count here that would have to agree with
                // it - `Boxed` and `Boxed(I32, I32)` are one mistake made twice.
                if(isGeneric(decl(), decl()[record]->gen)) {
                    return instantiateRecord(context, program, record, nullptr, 0, type.source);
                }

                return asBase<Type>(record);
            }

            context.diagnostics.error("unknown type %@"_v, type.source, context.findName(type.name));
            return nullptr;
        }
        case ast::Type::Gen: {
            /*
             * A lowercase name where a type is - a type variable of the declaration being read.
             *
             * The context decides whether writing one *declares* it: a function's is open while its
             * signature is read, so `fn id(->x: a)` introduces `a` by writing it, and a `data` head
             * lists its parameters, so a name that is not in the list is a name nothing declared.
             * Both answers arrive as null here and both are the same message, because from the use
             * site they are the same mistake - this name stands for nothing.
             */
            auto variable = genVariable(program, genContext, type.name, type.source);
            if(variable) return variable;

            context.diagnostics.error("%@ is not a type this declaration binds"_v, type.source,
                                      context.findName(type.name));
            return nullptr;
        }
        case ast::Type::App: {
            /*
             * A type application in a position with no ownership schema scope - a pointee, a type
             * argument of another application.
             *
             * The shape a loan argument would produce has nowhere to be numbered here, so null slots
             * is what refuses one; the shape that comes back is therefore always empty, which is
             * asserted rather than assumed - a dropped shape is the qualification a use site wrote
             * going missing, which is exactly what `Occurrence::shape` exists to carry.
             */
            DeclPtr<Type> logical;
            DeclPtr<OwnershipSchema> shape;
            if(!resolveApplication(type, Position::Field, nullptr, logical, shape)) return nullptr;

            assertTrue(Shapes::isEmpty(shape));
            return logical;
        }
        case ast::Type::Ptr: {
            auto to = resolveType(*parse[type.to]);
            return to ? program.types.pointerType(to) : nullptr;
        }
        case ast::Type::Fun: {
            /*
             * `(A, B) -> C` - the type a function value has, G7's row in type.def.
             *
             * Each position is resolved by `resolveOccurrence`, which is what a *declared* parameter
             * and result go through, so the two spellings of one shape agree by construction rather
             * than by two walks that read the sigils alike. Its `Position` argument is what makes a
             * parameter default to a borrow and the result to an owner, which is the same pair of
             * defaults a `fn` header has.
             *
             * The slot scope is **fresh and then required to be empty**. An origin published inside
             * a function type is numbered in that type's own scope - the value's signature is what
             * relates its parameters to its result - and a scope that is a local of this function
             * would be a numbering nothing carries. Refusing it here is one message at the position
             * that wrote it; letting it through would be a slot index meaning one thing in the type
             * and another in whatever declaration the type appears in.
             */
            auto& written = *parse[type.fun];
            if(written.kind != ast::FunKind::Plain) {
                unsupported(type.source, "a lens or iterator as a function type"_v);
                return nullptr;
            }

            SlotScope slots;
            SmallArray<FunParam, 4> params;

            for(auto argument: written.args.contents(parse)) {
                Occurrence occurrence;
                if(!resolveOccurrence(argument.type, argument.bind, Position::Parameter,
                                      slots, occurrence)) {
                    return nullptr;
                }

                params.push(FunParam { occurrence });
            }

            Occurrence result;
            if(!resolveOccurrence(written.ret, written.retBind, Position::Result, slots, result)) {
                return nullptr;
            }

            if(slots.count()) {
                unsupported(type.source, "an origin published inside a function type"_v);
                return nullptr;
            }

            return program.types.funType(decl(), params.pointer(), params.size(), result);
        }
        case ast::Type::Arr: {
            /*
             * `[T]` and `[T *n]` - one grammar, two types, and which one is which is whether a count
             * was written.
             *
             * `[T]` is the growable array, which is a *declaration* rather than a kind here: the
             * spec says "`[a]` and `Array(a)` are one type", so the bracket form resolves to what
             * `@builtin(array)` claimed and the two spellings intern to one handle. What makes a
             * parameter written `[T]` a *window* is not this function - a window is an access form
             * on the occurrence, and `resolveOccurrence` is where a position's contract is read.
             *
             * `[T *n]` is `ArrayType`, and the count is part of the type. See type.def.
             */
            auto element = resolveType(*parse[type.arr.type]);
            if(!element) return nullptr;

            // The element is a value's type in both, so `[{} *4]` and `[4]` are refused where they
            // are written rather than at the position that would have had to lay one out.
            if(!requireValueType(element, parse[type.arr.type]->source)) return nullptr;

            if(!type.arr.length) return sequenceType(element, type.source);

            DeclPtr<Type> count;
            U64 length = 0;
            if(!resolveArrayCount(*parse[type.arr.length], count, length)) return nullptr;

            if(count) return program.types.arrayType(decl(), element, count);
            return program.types.arrayType(decl(), element, length);
        }
        case ast::Type::Tup: {
            /*
             * `{I32, I32}` and `{x: I32, y: I32}` - `doc/spec/types.md`'s anonymous record.
             *
             * A **structural** type: what it is is its field list, so the same list written in two
             * signatures is one handle - see `RecordType::tuple`, where the argument for a tuple
             * being a record rather than a kind of its own is written out.
             *
             * `{}` is not one of these. The parser writes the empty braces as `Type::Unit`, which is
             * right: a product of nothing has exactly one value, and `{}` is the name the language
             * gives that type everywhere else.
             *
             * A field **default** is refused. `data` is where a default is written, because the
             * declaration is what a construction that omits the field reads it from; a tuple has no
             * declaration, so a default here would belong to one occurrence of the type and not to
             * the type - and two positions spelling the same list would then be one type with two
             * answers about what an omitted field holds.
             */
            SmallArray<RecordField, 4> fields;
            auto ok = true;

            for(auto written: type.tup.fields.contents(parse)) {
                if(written.def) {
                    context.diagnostics.error(
                        "a tuple field takes no default - a default belongs to the declaration a construction reads it from, and a tuple has none"_v,
                        parse[written.def]->source);
                    ok = false;
                    continue;
                }

                auto field = resolveType(written.type);
                if(!field) { ok = false; continue; }
                if(!requireValueType(field, written.type.source)) { ok = false; continue; }

                RecordField row;
                row.name = written.name;
                row.occurrence = owned(field);
                row.source = written.type.source;
                fields.push(row);
            }

            if(!ok) return nullptr;
            if(!uniqueTupleLabels(fields.pointer(), fields.size())) return nullptr;

            return tupleType(context, program, fields.pointer(), fields.size(), type.source);
        }
        case ast::Type::ArrInferred:
            /*
             * `[T *_]` in a position with no literal to count - `doc/spec/expressions.md`.
             *
             * Reached only where nothing supplies a count, and that is the whole of the check: the
             * two positions that do supply one - the `::` of an expression and the `::` of a
             * constant - go through `resolveAscribedType`, which answers before this function is
             * asked. So arriving here *is* the condition rather than something tested for, and a
             * parameter, a field and a return type land on it by having no `::` around them.
             */
            context.diagnostics.error(
                "a count written `_` is taken from the literal the type is written at, and there is no literal here - write the number, or move the type onto a `::` in front of one"_v,
                type.source);
            return nullptr;
        case ast::Type::Loan:
            /*
             * A bare origin where a type is - `Reader(a')`'s argument, read one level up.
             *
             * The parser produces it anywhere a type argument may stand and `resolveQualified` is
             * the only reader, which is the division `Kind::Lit` is already under: the position
             * takes a type, this is the one thing written there that is not one, and saying so here
             * is what keeps it from becoming a type by accident.
             */
            context.diagnostics.error(
                "an origin fills a slot rather than a type parameter, so it belongs in a type application"_v,
                type.source);
            return nullptr;
        default:
            unsupported(type.source, "this type form"_v);
            return nullptr;
    }
}

/*
 * `Array(T)` - the growable array, however it was spelled.
 *
 * `[T]` in a type position and `Array(T)` written by name reach the same instantiation, which is
 * what `doc/spec/collections.md`'s "`[a]` and `Array(a)` are one type" means at the level of
 * handles. Nothing here decides whether the occurrence using it is an owner or a window; that is the
 * *position*'s answer, and `resolveOccurrence` gives it.
 *
 * Reported rather than asserted where no declaration claimed the role, because a program compiled
 * against a library with no `@builtin(array)` is a broken installation and not a broken program -
 * `checkRoles` says so once, and this is what a use site says while that report is on its way.
 */
DeclPtr<Type> Resolver::sequenceType(DeclPtr<Type> element, LocationId source) {
    auto declaration = program.roles.record(intrinsic::Known::array);
    if(!declaration) {
        context.diagnostics.error(
            "the standard library declares no growable array, so `[T]` names nothing here"_v, source);
        return nullptr;
    }

    return instantiateRecord(context, program, declaration, &element, 1, source);
}

/*
 * The `n` of `[T *n]` - a written number, or a const parameter standing for one.
 *
 * Two answers rather than one type, and `ArrayType` says why it holds the number as a number: a
 * `ConstType` in the field would have to name an integer type, and every answer to which one is
 * arbitrary - so a written `[I32 *4]` would intern as a third type
 * that matches neither `[T *n]` at `n: I32` nor at `n: I64`.
 *
 * An expression rather than a type, because that is what the grammar has: `*n` is written outside
 * the brackets, so the parser reads it the way it reads any other expression. What is accepted is
 * the two forms a count can be - `doc/spec/types.md` records the rest as underspecified - and a
 * computed one is refused by name rather than approximated, since `[a *(n+1)]` and `[a *(1+n)]`
 * would intern as two types until there is a normal form for a const expression.
 */
bool Resolver::resolveArrayCount(ast::Expr& written, DeclPtr<Type>& count, U64& length) {
    count = nullptr;
    length = 0;

    if(written.kind == ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
        auto value = written.lit.i();

        if(!checkFixedArrayLength(value, written.source)) return false;

        length = U64(value);
        return true;
    }

    if(written.kind == ast::Expr::Var) {
        auto variable = genVariable(program, genContext, written.var, written.source);

        if(!variable) {
            context.diagnostics.error("%@ is not a count this declaration binds"_v, written.source,
                                      context.findName(written.var));
            return false;
        }

        // A *const* parameter, which is the whole of what `n: I32` on a head declares. A type
        // parameter written here would be a count that is a type, and the position that would have
        // had to multiply by it is a long way from the one that could say so.
        if(!((VariableType*)decl()[variable])->isConst()) {
            context.diagnostics.error(
                "%@ is a type parameter, and a count is a number - declare it `%@: Int`"_v,
                written.source, context.findName(written.var), context.findName(written.var));
            return false;
        }

        count = variable;
        return true;
    }

    context.diagnostics.error(
        "a fixed array's count is a literal or a const parameter, and this is neither"_v,
        written.source);
    return false;
}

/*
 * The one bound on how long a fixed array may be - `kMaxFixedArrayLength`, where it is argued.
 *
 * A bound on the count and not on the byte size, because the byte size is the target's answer and
 * this is a logical type. What it keeps out is a length whose derived teardown would be a walk no
 * machine finishes.
 *
 * One function because the count reaches the type from two directions - written as `*70000`, or
 * counted off the literal at `*_` - and a bound that is stated twice is a bound two spellings of one
 * array can disagree about.
 *
 * Signed, so that the *written* count's other way of not being a length is the same test and the
 * same message: `value` is what the literal said, a number too large to be an `I64` arrives here
 * negative, and both ends of "this is not a count" are one sentence naming the bound.
 */
bool Resolver::checkFixedArrayLength(I64 length, LocationId source) {
    if(length >= 0 && U64(length) <= kMaxFixedArrayLength) return true;

    context.diagnostics.error("a fixed array holds at most %@ elements, and this asks for %@"_v,
                              source, kMaxFixedArrayLength, length);
    return false;
}

/*
 * The type written on a `::`, which is the one position where `[T *_]` has something to count.
 *
 * `doc/spec/expressions.md`: "a count written `_` is taken from the literal", and "`_` is legal only
 * as the ascribed type of an array literal, since that is the only position with a literal to
 * count". This is that position, and it is the only reader of `ArrInferred` - every other type
 * position calls `resolveType`, whose arm for it is the report.
 *
 * Both `::`s go through here - the one in an expression and the one in a constant - because they are
 * one construct, and what `_` means cannot depend on which of the two a literal was written under.
 *
 * A **fill** needs no case of its own, and that is the spec's "the two spellings are complementary
 * rather than composable": `[0] :: [U8 *_]` is a one-element array, because the count a fill would
 * spread into is the one being read off the literal. Reading it as the number of elements written is
 * what makes that true rather than something checked for.
 */
DeclPtr<Type> Resolver::resolveAscribedType(ast::Type& written, ast::Expr& value) {
    if(written.kind != ast::Type::ArrInferred) return resolveType(written);

    /*
     * An array literal and nothing else.
     *
     * Not unwrapped through `Nested` or anything else, deliberately: what the count is read off is
     * the literal this type is written at, and a value with something around it is a value the
     * author put something around. `5 :: [Int *_]` is the case the message is for.
     */
    if(value.kind != ast::Expr::Array) {
        context.diagnostics.error(
            "a count written `_` is taken from the array literal the type is written at, and this is not one - write the number instead"_v,
            written.source);
        return nullptr;
    }

    // The element half is `resolveType`'s own `Arr` arm, asked the same two questions in the same
    // order - a `[{} *_]` is refused where it is written rather than at the position that would have
    // had to lay one out.
    auto element = resolveType(*parse[written.arr.type]);
    if(!element) return nullptr;
    if(!requireValueType(element, parse[written.arr.type]->source)) return nullptr;

    auto length = I64(value.arr.size());
    if(!checkFixedArrayLength(length, written.source)) return nullptr;

    return program.types.arrayType(decl(), element, U64(length));
}

DeclPtr<Type> Resolver::resolveConstParameterType(ast::Type& written) {
    auto type = resolveType(written);
    if(!type) return nullptr;

    if(decl()[type]->kind != Type::Int) {
        context.diagnostics.error(
            "a const parameter counts, so it is one of the integer types and this names %@"_v,
            written.source, typeText(context, decl(), type));
        return nullptr;
    }

    return type;
}

DeclPtr<Type> Resolver::resolveTypeArgument(ast::Type& written, DeclPtr<GenContext> head,
                                            Size position) {
    if(written.kind != ast::Type::Lit) return resolveType(written);

    /*
     * A number, which takes its type from the parameter it fills.
     *
     * The one argument form that cannot be resolved on its own, and the reason `resolveType`
     * refuses it: `4` written in a type position is a value, and what a value *is* here is decided
     * by the declaration being applied rather than by the literal. So a head that has no const
     * parameter at this position is what makes it a mistake, and saying which position is what
     * makes the message actionable.
     */
    auto parameter = headParameterOf(decl(), head, position);
    auto annotation = parameter ? ((VariableType*)decl()[parameter])->constType : nullptr;

    if(!annotation) {
        context.diagnostics.error("a number fills a const parameter, and this position takes a type"_v,
                                  written.source);
        return nullptr;
    }

    auto literal = parse[written.lit];
    if(!literal || literal->kind != ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
        context.diagnostics.error("a const argument is an integer literal"_v, written.source);
        return nullptr;
    }

    /*
     * Bounded by the parameter's own width, which is the whole of what the annotation buys: `4` at
     * an `I8` parameter and `4` at an `I32` one are two arguments, so a value that does not fit is
     * a mistake about which parameter this is rather than about the number.
     *
     * `minBits`, so that a const argument at a target width has to fit on **every** target. The
     * alternative is a program that compiles here and is a diagnostic on a narrower machine, which
     * is exactly what a bound the language states rather than the target is for.
     */
    auto value = literal->lit.i();
    auto& width = *(IntType*)decl()[annotation];
    auto bits = width.minBits();
    auto limit = bits == 64 ? ~U64(0) : ((U64(1) << (bits - (width.isSigned ? 1 : 0))) - 1);

    if(value > limit) {
        context.diagnostics.error("%@ does not fit in %@, which is what this parameter counts in"_v,
                                  written.source, value, typeText(context, decl(), annotation));
        return nullptr;
    }

    return program.types.constType(value, annotation);
}

/*
 * Whether a tuple's labels are distinct - asked of the finished field list, wherever one is made.
 *
 * Two fields of one name is a position no selector can reach: a named field is selected by its name
 * and by nothing else, and `.0` reaches an *anonymous* field only. So the second of them would be a
 * slot with no spelling, in a type that is otherwise indistinguishable from one with a single field.
 *
 * Here rather than at each site that builds a list, because there are two of them and they are the
 * two halves of one type: `{x: I32, x: I32}` written as a signature and `{x: 1, x: 2}` written as a
 * value make the same type, and a rule only the first obeyed would refuse a type the second can
 * still produce. The substituted and decomposed lists reach `tupleType` without coming through here,
 * which is right - both are made out of a list this already judged.
 */
bool Resolver::uniqueTupleLabels(const RecordField* fields, Size count) {
    auto ok = true;

    for(Size i = 0; i < count; i++) {
        if(fields[i].name == StringId()) continue;

        for(Size j = 0; j < i; j++) {
            if(fields[j].name != fields[i].name) continue;

            context.diagnostics.error("this tuple already has a field called %@"_v,
                                      fields[i].source, context.findName(fields[i].name));
            ok = false;
            break;
        }
    }

    return ok;
}

bool Resolver::requireValueType(DeclPtr<Type> type, LocationId source) {
    if(!type) return false;

    auto& written = *decl()[type];

    if(written.kind == Type::Const) {
        context.diagnostics.error("%@ is a number rather than a type, so nothing has it"_v, source,
                                  typeText(context, decl(), type));
        return false;
    }

    if(written.kind == Type::Variable && ((VariableType&)written).isConst()) {
        context.diagnostics.error("%@ is a const parameter, so it counts rather than classifies"_v,
                                  source, context.findName(((VariableType&)written).name));
        return false;
    }

    return true;
}

/*
 * A constant expression, which is what a global's initializer is.
 *
 * `hint` is the type the position asks for, and it is an input rather than a check afterwards:
 * `42` has no width of its own, and `let x = 42 :: I32` says what it is by ascription rather than
 * by having a later use decide. Inference here is deliberately local, the way the language's is.
 */
Constant Resolver::resolveConstant(ast::Expr& expr, DeclPtr<Type> hint, bool* notConstant) {
    // "This form is not a constant", answered rather than reported where the caller asked to be told
    // - see the declaration. One lambda so that the three refusals below stay one decision.
    auto notOne = [&]() -> Constant {
        if(notConstant) *notConstant = true;
        else unsupported(expr.source, "this constant expression"_v);

        return Constant();
    };

    // A literal's kind is `Lit` plus the literal kind, so it is a range rather than one label -
    // see `ast::Expr::Lit`, which the AST requires to be last for exactly this.
    if(expr.kind >= ast::Expr::Lit) return resolveLiteral(expr, hint, false);

    switch(expr.kind) {
        case ast::Expr::Coerce: {
            // `resolveAscribedType` rather than `resolveType`, which is what makes `[3, 1, 4] ::
            // [U8 *_]` mean the same thing in a global's initializer as in a body.
            auto& coerce = *parse[expr.coerce];
            auto type = resolveAscribedType(coerce.type, coerce.target);
            if(!type) return Constant();

            return resolveConstant(coerce.target, type, notConstant);
        }
        case ast::Expr::Nested:
            return resolveConstant(*parse[expr.nested], hint, notConstant);
        case ast::Expr::Con: {
            // A constructor whose value is a tag, which fits the eight bytes a `Constant` holds;
            // and then one whose value is its fields. Two forms rather than one because a value
            // position asks only for the first - see `resolveAggregateConstant`.
            if(auto constant = resolveConstructorConstant(expr, hint)) return constant.unwrap();

            auto aggregate = resolveAggregateConstant(expr, hint);
            if(!aggregate) return notOne();

            return aggregate.unwrap();
        }

        /*
         * `-1` is a written negative number, not a negation of a positive one.
         *
         * The lexer produces the magnitude alone, so folding the sign in here is what makes
         * `I64`'s own minimum expressible: negating it after the fact is signed overflow, and
         * `resolve/const.cpp` records that having been the bug on the other side.
         */
        case ast::Expr::Prefix: {
            if(isNegation(context, parse, expr)) {
                return resolveLiteral(parse[expr.prefix]->on, hint, true);
            }

            return notOne();
        }
        default:
            return notOne();
    }
}

/*
 * `= expr` on a field or on a parameter - `doc/spec/declarations.md`'s Field defaults, which
 * `doc/spec/functions.md` points a default argument at rather than restating.
 *
 * One function for both because it is one rule: a default is a **constant** of the position's own
 * type, built afresh wherever the site leaves the position out. The two callers differ in nothing
 * but the words, which is what `what` and `decides` carry - and in what they then do with the
 * answer, which is nothing at all until a construction or a call omits the position.
 *
 * The declared type is the hint rather than a check afterwards, for `resolveConstant`'s reason: `0`
 * has no width of its own. It is *also* checked afterwards, and that is not a duplicate - an
 * ascription inside the expression replaces the hint outright, so `x: I32 = 0 :: I8` resolves
 * happily and declares a field whose default is a different type than the field.
 *
 * Reported at the **expression** and not at the position it is on, which is where the mistake is: a
 * declaration whose type is right and whose constant is not is one caret's worth of wrong.
 */
Constant Resolver::declareDefault(ast::Expr& expr, DeclPtr<Type> type, StringId name,
                                  StringView what, StringView decides) {
    /*
     * A position declared as a type variable, which is the one refusal both callers share.
     *
     * A constant has a type and this position does not have one yet: `data Boxed(a) {value: a = 0}`
     * would need a `0` of whatever each `Boxed(T)` turned out to be. Refused by naming the variable,
     * because the fix is to give the position a type rather than to write a different constant.
     */
    if(type && decl()[type]->kind == Type::Variable) {
        context.diagnostics.error(
            "%@ is declared as %@, so a default here would be a constant of a type each %@ decides"_v,
            expr.source, context.findName(name), typeText(context, decl(), type), decides);
        return Constant();
    }

    auto value = resolveConstant(expr, type);
    if(!value.isSet()) return Constant();

    if(type && !sameType(value.type, type)) {
        context.diagnostics.error("this default is %@ and the %@ %@ is %@"_v, expr.source,
                                  typeText(context, decl(), value.type), what,
                                  context.findName(name), typeText(context, decl(), type));
        return Constant();
    }

    return value;
}

/*
 * `Off`, `True` - a constructor that carries nothing, which is a **compile-time value**.
 *
 * The one construction that folds to a constant, and generally rather than for `Bool`'s sake: a
 * constructor with no fields has nothing a site could vary, so writing one is naming a value. That
 * is what lets a field default, a short-circuit arm and an argument all say `True` without three
 * paths, and it is why `Bool` needs nothing said about it here - it is an enum the library declared
 * and its two constructors are nullary like any other's.
 *
 * Three answers rather than two, because the callers want different things from the middle one.
 * `Nothing` is "not this form, build it as a construction"; `Just` an unset constant is "it was this
 * form and something about it is wrong, already reported"; `Just` a set one is the value. A value
 * position falls through on the first, and a constant position reports on it.
 *
 * **Concrete and enum-layout**, which are the two limits. A generic `data Opt(a) = None | Some(a)`
 * has no type for `None` until something binds `a`, and a record whose other constructors carry
 * fields is reached through storage - neither is a value eight bytes can hold.
 */
Maybe<Constant> Resolver::resolveConstructorConstant(ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.con];
    if(written.args.size() || written.type.kind != ast::Type::Con) return Nothing();

    auto found = findConstructor(context, program, here(), written.type.name, expr.source);
    if(!found) return Nothing();

    auto& record = *decl()[found.record];
    if(record.gen || record.layout != RecordLayout::Enum) return Nothing();

    auto type = asBase<Type>(found.record);
    if(!reachesInside(record, expr.source, "its constructor"_v)) return Just(Constant());

    // The same refusal `resolveConstruction` makes in the same words, since it is the same mistake:
    // what this builds is decided by the constructor, and the position asked for something else.
    if(hint && !sameType(hint, type)) {
        context.diagnostics.error("this position asks for %@, and this builds %@"_v, expr.source,
                                  typeText(context, decl(), hint), typeText(context, decl(), type));
        return Just(Constant());
    }

    return Just(enumConstant(type, found.index));
}

/*
 * `Circle(7)` and `Point {x: 1, y: 2}` - a record built out of constants.
 *
 * The other constant form a construction has, and a separate function rather than a branch in the
 * one above because the **callers** are not the same. A value position asks that one and not this:
 * what it is looking for is a constructor a register can hold, and a record is storage, so the
 * construction it would otherwise build is what it wants. This is asked where a constant is what the
 * position is *for* - a global's initializer, a field default, a parameter default.
 *
 * That is a gap and not a design. A body's `Point {x: 1, y: 2}` reduces to exactly this value and
 * `Constant` was widened so that it could, but folding it there costs two things this stage has not
 * got: the walk below is a **speculative** one in a value position, so an inner report leaks - the
 * widening a field's value is entitled to happens in `coerce` and not here, and `Point {x: 1 :: I8}`
 * would be refused rather than widened - and the operand nodes a construction has already appended
 * would be left dead, which is a `Const` nothing names in a stage that runs no pass to remove one.
 * Both are the same missing piece from two sides: a constant position and a value position have to
 * reduce by one rule, and that rule has to run where the value is made.
 *
 * Three answers, the same three the enum form gives - see it, where the protocol is stated.
 * `Nothing` is "not this form", and a field whose value is not a constant is what makes it one:
 * the position then says so about the expression the author wrote rather than about the field this
 * walk happened to reach first.
 */
Maybe<Constant> Resolver::resolveAggregateConstant(ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.con];
    if(written.type.kind != ast::Type::Con) return Nothing();

    auto found = findConstructor(context, program, here(), written.type.name, expr.source);
    if(!found) return Nothing();

    auto& record = *decl()[found.record];

    /*
     * A **generic** declaration, which is the one limit left.
     *
     * `data Opt(a) = None | Some(a)` has no type for `None` until something binds `a`, and what
     * would bind it here is the inference a construction runs over its field values - which is a
     * `TypeArguments` walk this position has no builder to make one with. So it comes back as "not
     * this form", and the position says what it says about anything else it cannot reduce.
     */
    if(record.gen) return Nothing();

    auto type = asBase<Type>(found.record);
    if(!reachesInside(record, expr.source, "its constructor"_v)) return Just(Constant());

    // The same refusal `resolveConstruction` makes in the same words, since it is the same mistake:
    // what this builds is decided by the constructor, and the position asked for something else.
    if(hint && !sameType(hint, type)) {
        context.diagnostics.error("this position asks for %@, and this builds %@"_v, expr.source,
                                  typeText(context, decl(), hint), typeText(context, decl(), type));
        return Just(Constant());
    }

    auto arm = record.cons.get(decl(), found.index);

    ArgMap map;
    if(!constructionArguments(expr, record, arm, map)) return Just(Constant());

    SmallArray<Constant, 8> values;
    for(U16 i = 0; i < arm.count; i++) values.push(Constant());

    /*
     * Every field, in written order and filling the field the mapping named - `resolveConstruction`'s
     * walk with `resolveConstant` where it has `resolveValue`, which is the whole difference between
     * a construction that runs and one that reduces.
     *
     * A field whose value is not a constant makes the whole construction **not this form**, which
     * is `Nothing` and not a report: in a value position that is the ordinary case and
     * `resolveConstruction` builds it, and in a constant one the caller says so at the expression
     * the author wrote rather than at the field it noticed first. So the flag the field walk passes
     * down is its own - it suppresses the inner report - and is told apart from a field that *is*
     * this form and is wrong, which reports and comes back unset.
     */
    auto ok = true;
    auto reduced = true;
    auto at = U32(0);

    // Where each written field was, so that a type mismatch is reported *after* the walk has settled
    // whether this is the form at all - see below. Read only for a field the construction wrote,
    // which `ArgMap::omitted` is what says; a field it left out takes the declaration's own constant
    // and was checked where that was declared.
    SmallArray<LocationId, 8> sources;
    for(U16 i = 0; i < arm.count; i++) sources.push(LocationId(expr.source));

    for(auto argument: written.args.contents(parse)) {
        auto index = map.position(at++);
        auto field = record.field(decl(), U16(arm.first + index));

        /*
         * A field that holds a **loan**, which no constant can fill: what would go in the storage is
         * a reference to something, and a constant is a value with nowhere for that something to be.
         */
        if(!field.occurrence.contract.isOwned()) {
            context.diagnostics.error(
                "field %@ refers to storage the record does not own, so it has no constant form"_v,
                argument.value.source, context.findName(field.name));
            ok = false;
            continue;
        }

        auto dynamic = false;
        auto value = resolveConstant(argument.value, field.occurrence.type, &dynamic);

        if(dynamic) { reduced = false; continue; }
        if(!value.isSet()) { ok = false; continue; }

        sources[index] = argument.value.source;
        values[index] = value;
    }

    // The fields the construction left out, each the field's own constant - the same line
    // `resolveConstruction` reaches, one tier down. A position with no default was reported by the
    // mapping, so what is read here is always set.
    for(U16 i = 0; i < arm.count; i++) {
        if(map.omitted(i)) values[i] = record.field(decl(), U16(arm.first + i)).def;
    }

    /*
     * "Not this form" answers before "this form is wrong", and the order is the whole of what keeps
     * a legal program compiling: a construction one of whose fields is a runtime value is built by
     * `resolveConstruction`, and that path *widens* a field's value where this one only compares -
     * so `Point {x: n, y: 2 :: I8}` would be refused here for a conversion the value path performs.
     * The same reasoning is why the field types are checked below rather than in the walk: a report
     * made before the form is settled is a report the value path then makes again.
     */
    if(!reduced) return Nothing();

    for(U16 i = 0; i < arm.count; i++) {
        // A field the walk already reported on has no value to compare, and saying so twice about
        // one field is the thing this second pass exists to avoid.
        if(map.omitted(i) || !values[i].isSet()) continue;

        auto field = record.field(decl(), U16(arm.first + i));
        if(sameType(values[i].type, field.occurrence.type)) continue;

        context.diagnostics.error("this field is %@ and %@ is %@"_v, sources[i],
                                  typeText(context, decl(), values[i].type),
                                  context.findName(field.name),
                                  typeText(context, decl(), field.occurrence.type));
        ok = false;
    }

    if(!ok) return Just(Constant());

    return Just(program.aggregateConstant(type, found.index, values.pointer(), values.size()));
}

/*
 * What a literal with nothing to go on is - the head default of the class that converts one.
 *
 * `class FromInt(a = Int)` and `class FromDecimal(a = Float)`, and the default is the statement
 * rather than a convenience: `1 + 1` writes neither class, so there is no argument position for one
 * to fill and what it answers is "nothing decided this, so what is it". Read rather than written
 * out here, because a compiler that wrote it out is a compiler that can come to disagree with the
 * library - and did, for `1.5`.
 *
 * Reported rather than asserted where the library states nothing, for `sequenceType`'s reason: a
 * program compiled against a library whose `FromDecimal` has a bare head is a broken installation
 * and not a broken program.
 */
DeclPtr<Type> Resolver::literalDefault(intrinsic::Known role, LocationId source) {
    if(auto type = classDefaultType(program, role)) return type;

    context.diagnostics.error(
        "the standard library's %@ says nothing about what a literal with no type around it is, so this one has none"_v,
        source, intrinsic::knownDef(role).role);
    return nullptr;
}

/*
 * A written number, read once - which conversion class it is an argument of, and the number as
 * `foldLiteral` and the call both take it.
 *
 * The sign is folded in here: the lexer produces the magnitude alone, and `-9223372036854775808`
 * has to be one constant rather than a negation of a magnitude the type cannot hold. False after
 * reporting a literal kind this stage has no class for.
 */
bool Resolver::writtenLiteral(ast::Expr& expr, bool negative, WrittenLiteral& into) {
    auto& literal = expr.lit;

    switch(ast::Literal::Kind(expr.kind - ast::Expr::Lit)) {
        case ast::Literal::Int: {
            auto magnitude = literal.i();
            into.role = intrinsic::Known::fromInt;
            into.bits = negative ? U64(0) - magnitude : magnitude;
            into.magnitude = magnitude;
            into.negative = negative;
            into.value = negative ? -double(magnitude) : double(magnitude);
            return true;
        }
        case ast::Literal::Double:
        case ast::Literal::Float:
            into.role = intrinsic::Known::fromDecimal;
            into.bits = 0;
            into.value = negative ? -literal.d() : literal.d();
            return true;
        default:
            unsupported(expr.source, "this literal"_v);
            return false;
    }
}

/*
 * A written number as a constant of the type it landed at - the fold, plus the one thing the fold
 * cannot do.
 *
 * `foldLiteral` puts the value in its type's normal form, which is what makes `Box {small: 20}` on a
 * `@bits(4)` field hold 4 rather than a number the type cannot represent. Saying so is *this*
 * function, because the fold is shared with an expansion that has no source position and no
 * diagnostics - and because a warning belongs where a **written** number is, once. Every other
 * producer of a constant hands over bits that are already at the type's width - a mask this stage
 * computed, a shift distance, a global's stored initializer - so warning there would be reporting
 * the compiler's own constants back at the author.
 *
 * A warning rather than an error, because the truncation is defined - `doc/spec/repr.md`: "store
 * always narrows, by an unconditional mask" - and because a full-width mask written at a signed
 * type, `0xFFFFFFFF :: Int`, is an idiom rather than a mistake. What it catches is the case with no
 * other symptom at all: nothing in `Box {small: 20}` says 4.
 */
Constant Resolver::foldWritten(const WrittenLiteral& written, DeclPtr<Type> type, LocationId source) {
    if(type && decl()[type]->kind == Type::Int) {
        auto& integer = *(IntType*)decl()[type];

        if(!integerHolds(integer, written.magnitude, written.negative)) {
            auto reduced = reducedToWidth(integer, written.bits);
            auto sign = written.negative ? "-"_v : ""_v;

            // Printed the way the type reads it, since the point of the message is what the program
            // will do and a signed truncation that comes out negative is the surprising case.
            if(integer.isSigned) {
                context.diagnostics.warning(
                    "the literal %@%@ does not fit in %@ and is truncated to %@"_v, source, sign,
                    written.magnitude, typeText(context, decl(), type), I64(reduced));
            } else {
                context.diagnostics.warning(
                    "the literal %@%@ does not fit in %@ and is truncated to %@"_v, source, sign,
                    written.magnitude, typeText(context, decl(), type), reduced);
            }
        }
    }

    return foldLiteral(decl(), type, written.bits, written.value);
}

// The argument the conversion class's member takes - `fromInt(value: Long)` reads a `Long`, and
// what type that is is the declaration's own parameter and not a width chosen here.
Constant Resolver::conversionArgument(const WrittenLiteral& written) {
    auto& member = *decl()[decl()[program.roles.klass(written.role)]->members.get(decl(), 0)];
    auto type = decl()[member.args.get(decl(), 0)]->occurrence.type;

    if(written.role == intrinsic::Known::fromInt) return intConstant(type, written.bits);
    return floatConstant(type, written.value);
}

DeclPtr<Instance> Resolver::literalInstance(intrinsic::Known role, DeclPtr<Type> type,
                                            LocationId source) {
    InstanceMatch found;
    if(!selectInstance(context, program, program.roles.klass(role), &type, 1, source, true, found)) {
        return nullptr;
    }

    return found.instance;
}

// The `provided.def` row whose instances a conversion class's are - the one place the two roles
// are paired, so that a literal asks the family the same question a generated instance answered.
static intrinsic::Provided providerOf(intrinsic::Known role) {
    return role == intrinsic::Known::fromInt ? intrinsic::Provided::fromInt
                                             : intrinsic::Provided::fromDecimal;
}

/*
 * `True` and `False` as `ast::Literal::Bool` - the parser's spelling, which the source has not got.
 *
 * `True` written by an author is a nullary constructor and resolves as one. What produces this kind
 * is `parseIfExpr`: an `if:` case written `else` is recorded as the condition that always holds, so
 * that a case list stays one production. This is that encoding's other half, and the only reader of
 * the kind below the parser.
 *
 * A **constant**, and that is what separates it from every other literal kind. A number is a call of
 * `FromInt` because the type it lands at decides what it means; truth has exactly one type, so there
 * is nothing to select and nothing to fold. So it answers ahead of `writtenLiteral`, which pairs
 * each kind it knows with a conversion class and has none for this one.
 */
Constant Resolver::truthLiteral(ast::Expr& expr) {
    if(!ast::isLiteral(expr)) return Constant();
    if(ast::Literal::Kind(expr.kind - ast::Expr::Lit) != ast::Literal::Bool) return Constant();

    return truthConstant(program, expr.lit.b);
}

/*
 * A literal in a **constant** position - a global's initializer, a field or parameter default.
 *
 * `doc/spec/core.md`: a literal is a call to `FromInt`. What a constant position can hold is the
 * one case where that call folds - the instance the compiler generated, whose `fromInt` of a
 * constant is a constant - and `foldLiteral` is the fold, shared with the expansion the value
 * path reaches. A declared instance is a call with a body, and a call is not a constant; said so
 * rather than folded past, because the author's `fromInt` is what they asked to run.
 *
 * Asked of the *family* rather than of the instance table, because a field default is resolved
 * before the classes have their members and therefore before any instance the compiler generates
 * exists - and the family is the same statement the generated instance is made from.
 */
Constant Resolver::resolveLiteral(ast::Expr& expr, DeclPtr<Type> hint, bool negative) {
    // `negative` cannot hold here - a `-` before a truth is a prefix over a constructor and never
    // reaches a literal at all - and it is asked anyway, so that this reads as the one kind it is
    // rather than as a kind that ignores half of what it was given.
    if(auto truth = truthLiteral(expr); !negative && truth.isSet()) return truth;

    WrittenLiteral written;
    if(!writtenLiteral(expr, negative, written)) return Constant();

    auto type = hint ? hint : literalDefault(written.role, expr.source);
    if(!type) return Constant();

    /*
     * `let &heapNext = 0 :: %U8` - the **zero** of a pointer, which is the one number one holds.
     *
     * A constant position is the bytes a global or a default has before anything runs, and those are
     * zero for every type; a pointer is the one type where saying so needs a rule, because there is
     * no `FromInt(%a)` and there should not be. What this admits is exactly the zero: any other
     * number would be an address this compiler invented, which is what `lib/Native/Pointer.yana`
     * argues no compiler should have an opinion about - `null()` is a *library* function over
     * `bitcast` for that reason, and this is not a second spelling of it.
     *
     * The value path is untouched, so `let p = 5 :: %U8` in a body is still the `FromInt` refusal
     * below - the shipping stage refuses the same expression in the same position.
     */
    if(decl()[type]->kind == Type::Pointer && written.role == intrinsic::Known::fromInt &&
       !negative && written.bits == 0) {
        return intConstant(type, 0);
    }

    if(!providedFor(program, providerOf(written.role), type)) {
        context.diagnostics.error(
            "a literal of %@ is a call of its %@ instance rather than a constant, and this position needs a constant"_v,
            expr.source, typeText(context, decl(), type),
            context.findName(decl()[program.roles.klass(written.role)]->name));
        return Constant();
    }

    return foldWritten(written, type, expr.source);
}

/*
 * `"abc"` - a string, from bytes the compiler places.
 *
 * Two things and only two are the compiler's here. The **bytes** are a constant global of
 * `[CodeUnit *n]`, laid out by the array's own manifestation and reached the way any global is - a
 * borrow, whose address `addressOf` publishes as rooted in a global, which outlives the program. The
 * **value** is whatever the library builds out of that address and a count: `stringLiteral`, the
 * function `known.def` names, whose body is where a run that owns nothing is described and whose
 * result is the `String` the role names. Nothing here spells a field of it.
 *
 * A global per literal rather than a constant of the string type, because a `String` holds an
 * address and a constant holds bytes - `materialize` writes no relocations - and because what the
 * library builds is the library's to change. The call is `@inline(always)` in the prelude, so what
 * a use of a literal costs is the stores the body writes and no call.
 */
Node* Resolver::stringLiteralValue(Builder& builder, ast::Expr& expr) {
    auto text = context.findName(expr.lit.s);
    auto count = U64(text.size());

    auto unit = program.types.codeUnit;
    auto bytes = program.types.arrayType(decl(), unit, count);

    Array<Constant> values;
    values.reserve(count);
    for(Size i = 0; i < count; i++) values.push(intConstant(unit, U8(text.text()[i])));

    auto global = new (program.decl) Global(program.uniqueSymbol(here(), context, "string"_v),
                                            owned(bytes));
    global->source = expr.source;
    global->initial = program.aggregateConstant(bytes, 0, values.pointer(), count);

    auto pointer = global - *program.decl;
    decl()[here()]->globals.push(program.decl, pointer);

    auto addressOf = program.roles.operation(intrinsic::Op::addressOf);
    if(!addressOf) {
        context.diagnostics.error(
            "the standard library does not declare anything with the role addressOf, which a string literal needs"_v,
            expr.source);
        return nullptr;
    }

    // A borrow of the global, at the published request `addressOf`'s parameter states - which is
    // what makes the read a loan rather than a copy of every byte.
    auto& taker = *decl()[addressOf];
    auto place = builder.globalPlace(pointer);
    auto borrowed = readPlace(builder, expr.source, *place,
                              requestOf(decl()[taker.args.get(decl(), 0)]->occurrence));
    if(!borrowed) return nullptr;

    Node* of[1] = { borrowed };
    auto address = callFunction(builder, taker, of, 1, expr.source);
    if(!address) return nullptr;

    auto units = builder.addBitcast(expr.source, *address, program.types.pointerType(unit));
    auto length = builder.addConst(expr.source, intConstant(program.types.words[1], count));

    Node* arguments[2] = { units, length };
    return callKnown(builder, intrinsic::Known::stringLiteral, arguments, 2, expr.source);
}

Node* Resolver::literalValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool negative) {
    if(auto truth = truthLiteral(expr); !negative && truth.isSet()) {
        return builder.addConst(expr.source, truth);
    }

    if(ast::Literal::Kind(expr.kind - ast::Expr::Lit) == ast::Literal::String) {
        if(negative) {
            context.diagnostics.error("a string cannot be negated"_v, expr.source);
            return nullptr;
        }

        return stringLiteralValue(builder, expr);
    }

    WrittenLiteral written;
    if(!writtenLiteral(expr, negative, written)) return nullptr;

    auto type = hint ? hint : literalDefault(written.role, expr.source);
    if(!type) return nullptr;

    return literalNode(builder, written, type, expr.source);
}

/*
 * A literal in a **value** position, at a type - the call, or the constant it folds to.
 *
 * At a concrete type whose instance is the compiler's the answer is the constant and nothing
 * else: no `Long` is built to be folded away, so `1 :: I32` is the one `const` it always was. At
 * any other type it is what the language says it is - `fromInt(written)` through the ordinary
 * call path, which selects the declared instance where the type is concrete and discharges
 * `FromInt(a)` against the enclosing signature where it is a variable. So `x + 1` in
 * `fn (Num(a)) f(x: a)` is a call `Num`'s superclass has already promised every instance can
 * answer, and `5 :: Money` runs the `fromInt` the author wrote.
 */
Node* Resolver::literalNode(Builder& builder, const WrittenLiteral& written, DeclPtr<Type> type,
                            LocationId source) {
    if(isConcreteType(decl(), type)) {
        auto instance = literalInstance(written.role, type, source);
        if(!instance) return nullptr;

        /*
         * The fold, which is what a generated instance's `fromInt` computes at a **scalar**.
         *
         * A vector's is not a constant: there is no vector constant in this IR (see
         * `expandVecIota`, where the reason is written), so `1 :: Vec(I32)` is the splat of the
         * lane's own constant and that is a node rather than a value. It reaches that node through
         * the ordinary call path below, where `expandProvided`'s vector arm folds at the lane and
         * splats - so the two spellings of `splat(1)` are one expansion and not two.
         */
        auto scalar = decl()[type]->kind == Type::Int || decl()[type]->kind == Type::Float;

        if(scalar && decl()[instance]->provided != intrinsic::Provided::Count) {
            return builder.addConst(source, foldWritten(written, type, source));
        }
    }

    auto& callee = *decl()[decl()[program.roles.klass(written.role)]->members.get(decl(), 0)];
    auto& parameter = *decl()[callee.args.get(decl(), 0)];

    // The class's variable is the *result* here - `fromInt(value: Long) -> a` - so it is bound
    // from the position before the argument is prepared, which binds nothing.
    TypeArguments typeArguments(*this, callee.gen);
    typeArguments.bind(callee.result.type, type);

    auto argument = builder.addConst(source, conversionArgument(written));

    SmallArray<Node*, 8> arguments;
    auto prepared = prepareArgument(builder, *argument, parameter, typeArguments, source);
    if(!prepared) return nullptr;

    arguments.push(prepared);
    return finishCall(builder, callee, arguments, typeArguments, nullptr, source);
}

/*
 * A name in scope, innermost first, down to the floor.
 *
 * The floor is zero in every ordinary body and is the whole of what a **lifted** body needs from
 * scoping - see `LambdaFrame`. A lambda's body is resolved inside the enclosing walk and must not be
 * able to name that function's locals as locals, so the backward walk stops rather than continuing
 * into a scope that belongs to a different declaration; what it names above the floor becomes a
 * capture instead.
 */
Local* Resolver::findLocal(StringId name, bool* bound) {
    for(Size i = bindings.size(); i > bindingFloor; i--) {
        if(bindings[i - 1].name != name) continue;

        if(bound) *bound = true;
        return bindings[i - 1].local;
    }

    return nullptr;
}

ModuleScope::ModuleScope(Resolver& resolver, DeclPtr<Module> module, U16 file):
    resolver(resolver), saved(resolver.current), savedFile(saved ? saved->activeFile : 0) {
    resolver.current = resolver.decl()[module];
    resolver.current->activeFile = file;
}

ModuleScope::~ModuleScope() {
    if(saved) saved->activeFile = savedFile;
    resolver.current = saved;
}

/*
 * The order the passes run in, which is not the order they were written in and is load-bearing at
 * four points.
 *
 *  - **Every module before any declaration.** An import may not trigger the resolution of what it
 *    names, or a cycle between two modules would mean whichever of them was compiled first
 *    (Analysis-Modules.md §2.2). `discoverModules` finds them all and `linkImports` links them, and
 *    only then does anything read a declaration.
 *  - **Types before anything that names one.** A `data` may hold a record declared below it, so the
 *    shells exist before any field type is resolved.
 *  - **Every declaration before `classifyTypes`.** Whether a type has a teardown depends on the
 *    instances the program wrote, and every read of a name in every body asks that question through
 *    `readMode`. A classification run before the last `instance` would answer differently for the
 *    bodies resolved before it.
 *  - **Every initializer before any body.** A `let`'s type is what its initializer says, and a body
 *    that reads the name needs it. One interleaved walk resolved them in source order, so a function
 *    above the `let` it read was resolved against a global with no type at all.
 *
 * Every phase runs over the whole program rather than over one module at a time, which is the same
 * statement the first point makes from the other side: a pass that finished a module before starting
 * the next would be an ordering between them, and a cycle has no ordering to be given.
 */
Ptr<Program> resolveProgram(Context& context, ast::ModuleGroup& root, ast::ModuleProvider* provider) {
    // The one invariant `compiler/intrinsic`'s four tables cannot state as a `static_assert`: no two
    // rows anywhere share a role name. A duplicate would make which of them `@builtin` reached
    // depend on the order the tables are searched, silently. Assertion builds only.
    intrinsic::verifyRoleTable();

    auto program = Ptr<Program>(new Program());

    Array<ast::ModuleGroup*> groups;
    discoverModules(context, *program, root, provider, groups);

    Resolver resolver { context, *program, *context.parseRegion, nullptr, toBuffer(groups) };

    // The edges, before any declaration is read - see the order above. Reported here rather than
    // during discovery, because an import that named nothing is a diagnostic about the line that
    // wrote it and discovery has no line in hand.
    linkImports(context, *program, toBuffer(groups));

    /*
     * `@platform`, judged before anything is read - see `checkPlatform`.
     *
     * Ahead of every declaring pass because those passes *skip* what this target has not got, so an
     * attribute on an excluded declaration would never be looked at: a typo that deleted a
     * declaration from every build would be the one thing that hid its own diagnostic.
     */
    resolver.eachFile([&](ast::Module& file) { resolver.checkPlatform(file); });

    /*
     * Four declaration phases, and the order is the dependency order.
     *
     * Record names, then their fields, then every record's ownership schema, then everything else -
     * because a signature may bind a record's interior positions (`x: Reader(a')`) and needs the
     * schema to exist, while the schema needs every field and nothing more. `classifyTypes` is last
     * of all, because what a type *owes* depends on the instances, which are declarations too.
     *
     * There is one error checkpoint and it is after all four, deliberately. Splitting the walk moved
     * every `data` diagnostic ahead of every `fn` one, which is a reordering a golden records; a
     * checkpoint *between* the phases would have made it a truncation instead, and a file with a bad
     * field would stop reporting anything about its functions. `foldSchemas` is safe to run over a
     * partly resolved record for the same reason: a field that failed to resolve is a field the
     * record does not have.
     */
    resolver.eachFile([&](ast::Module& file) { resolver.declareFixities(file); });
    resolver.eachFile([&](ast::Module& file) { resolver.declareTypes(file); });
    resolver.eachFile([&](ast::Module& file) { resolver.declareAliases(file); });

    // The class heads, before the fields: a field type cannot name a class, but a *member* signature
    // can name any class and an `instance` head names one, so every class exists before either.
    resolver.eachFile([&](ast::Module& file) { resolver.declareClasses(file); });

    /*
     * Every generic head's annotations and defaults - `Resolver::PendingHead`.
     *
     * Here because this is the first point at which the type namespace is whole: every record, every
     * alias and every class is a name, and nothing after this adds one. Before the fields, because a
     * field type applies a head - `{v: Buf}` at `data Buf(n: Int = 4)` - and an application that
     * omits an argument reads the default, while `[a *n]` reads the annotation to know that `n`
     * counts.
     *
     * The three shell passes above each used to read their own head, which put two of them in a pass
     * that had not built the namespace yet: `n: Int` was an unknown type where `n: I32` was not, and
     * `data Foo(a = Bar)` resolved or not depending on which of the two was written first.
     */
    resolver.resolveHeads();

    resolver.eachFile([&](ast::Module& file) { resolver.declareRecordFields(file); });

    /*
     * The instantiations that walk made of records it had not read yet - `completeInstantiations`.
     *
     * The same hazard the alias note below states, at the one place ordering cannot reach it: a
     * field's type is resolved *inside* this pass, so `data Node {leaf: Maybe(Int)}` makes a
     * `Maybe(I32)` whose declaration may not have been read, and no order of the two would fix a
     * program that declares them the other way round.
     */
    completeInstantiations(context, *program);

    // The field defaults the walk above queued, now that every constructor in the program exists -
    // `Resolver::PendingDefault`, where the ordering this closes is argued.
    resolver.resolveFieldDefaults();

    /*
     * Every alias's target, after the fields and before anything else reads one.
     *
     * After, because a target that applies a generic record - `alias Twin(a) = Two(a, a)` - makes an
     * *instantiation*, and `instantiateRecord` copies the declaration's fields at the moment it runs
     * (`gen.cpp`). An alias resolved before `Two`'s fields were read would produce a `Two(I32, I32)`
     * with none, and every projection through the alias would then report a field the author wrote.
     *
     * A use site reaches `aliasTarget` on its own, so this pass is what makes an alias nothing names
     * still be checked - the same reason a record's fields are read whether or not anything holds
     * one.
     */
    resolver.resolveAliasTargets();

    foldSchemas(context, *program);

    resolver.eachFile([&](ast::Module& file) { resolver.declareClassMembers(file); });

    /*
     * The compiler's roles, the half that needs only the declarations - `intrinsic/link.h` - and
     * then the instances the compiler generates from them.
     *
     * Here and not with the rest of `checkRoles`, because `provideInstances` reaches every class
     * the tables name by member position, and a class of the wrong shape has to be refused before
     * anything indexes it. Before `declare`, because a written `instance` may need a generated one
     * for its superclass - `instance Ord2(I32)` over a class requiring `Eq` finds `Eq(I32)` in the
     * table only if it is already there.
     */
    if(!checkDeclaredRoles(context, *program)) return program;
    resolver.provideInstances();

    resolver.eachFile([&](ast::Module& file) { resolver.declare(file); });

    /*
     * `pub`, from the declaring side - `export.cpp`.
     *
     * Here because an exported *interface* is finished here: every signature, every field and every
     * global's type is in, and a body has nothing to say about who may name what. Before the
     * checkpoint below rather than after it, so that a leak is reported alongside the declaration
     * diagnostics it belongs with instead of being reached only by a program that had none.
     */
    checkExports(context, *program, toBuffer(groups));

    if(context.diagnostics.errorCount()) return program;
    if(!classifyTypes(context, *program)) return program;

    /*
     * What a qualified type argument may be written over - `checkQualifiedShapes`.
     *
     * Immediately after the classification and before the bodies, because that is the first point
     * the answer exists and the last point before anything is built against it: what a `Maybe('v)`
     * releases is a question about `Maybe`'s members, and whether `Maybe` has any is what the pass
     * above settles.
     */
    if(!checkQualifiedShapes(context, *program)) return program;

    resolver.eachFile([&](ast::Module& file) { resolver.resolveInitializers(file); });

    // The half of `pub` that had to wait for them - a global's type is what its initializer said.
    checkExportedGlobals(context, *program);

    /*
     * The compiler's roles against the declarations that claimed them - `intrinsic/link.h`.
     *
     * After the initializers, because a supplied global's check reads the constant its initializer
     * settled on and writes the target's answer over it. Before the bodies, because everything a
     * body does with a role - calling an intrinsic, reaching a teardown - relies on the claimant
     * being the shape the compiler assumes, and a body resolved against a wrong one would report the
     * mistake at the call site instead of at the declaration.
     *
     * A failure stops here rather than being counted and carried on with: what it says is that this
     * compiler and this library disagree, and every diagnostic a body then produced would be about
     * that rather than about the program.
     */
    if(!checkRoles(context, *program)) return program;

    // The bodies of the members the compiler generated, on the same terms as every other body -
    // after the types are classified, because reading a parameter asks `readMode`.
    resolver.provideBodies();

    /*
     * Where the program starts, and the body that runs before it - Analysis-Initialization.md's
     * stage B.
     *
     * **Before every other body**, which is the ordering this pass exists at: a dynamic global's
     * type is what its initializer turned out to be, and a function above the `let` it reads needs
     * that answer. It is the same statement `resolveInitializers` makes about a constant one, made
     * about the half that is code.
     */
    resolver.resolveProgramEntry();

    resolver.eachFile([&](ast::Module& file) { resolver.resolveBodies(file); });

    // The one question a class declaration cannot be asked before its defaults have bodies - see
    // `checkClassDefaults`. Here rather than inside the walk above, because a default may call a
    // member declared below it and a per-declaration check would answer against half a class.
    resolver.checkClassDefaults();

    /*
     * The canonical slot numbering, derived once every context is finished - `gen.h`.
     *
     * After the bodies and not after the signatures, because a body is the other source of
     * requirements: what a generic body dispatches on is part of its environment, and a numbering
     * frozen before the body had been read would be one the body then invalidated. Nothing this
     * milestone builds adds a requirement from a body, and the call is placed where it will still be
     * right when something does - which is the point of freezing at all rather than caching.
     */
    freezeSchemas(*program);

    if(context.diagnostics.errorCount()) return program;

    /*
     * Verified, then analysed, then summarized, and the order is the dependency order.
     *
     * `verifyProgram` runs first because the two passes after it walk the IR and neither has a
     * position to report a malformed one at. `analyzeProgram` runs before `computeSummaries` because
     * reachability follows the teardowns it places: an `instance Drop(T)` is reached by an object
     * dying rather than by a call, so a walk that only followed calls would leave its body
     * unconverted and the drop with nothing to call.
     */
    if(!verifyProgram(context, *program)) return program;
    if(!analyzeProgram(context, *program)) return program;

    /*
     * Which form each generic body is compiled in - `selectGenForms`.
     *
     * After the analysis because a teardown is part of what an erased body would have to do and is
     * placed there, and before the summaries because that is the walk which specializes: the form is
     * what will decide whether a call makes a specialization at all, so it has to be settled before
     * anything asks.
     */
    selectGenForms(context, *program);
    if(context.diagnostics.errorCount()) return program;

    computeSummaries(context, *program);

    // Every specialization the walks above made, against the generic it came from - `verify.h`. Last
    // because it is the only check whose subject does not exist until the walks have run.
    checkSpecializations(context, *program);
    return program;
}

Ptr<Program> resolveProgram(Context& context, ast::Module& root, ast::ModuleProvider* provider) {
    ast::ModuleGroup group { root.name };
    group.files.push(&root);
    return resolveProgram(context, group, provider);
}

} // namespace mir
