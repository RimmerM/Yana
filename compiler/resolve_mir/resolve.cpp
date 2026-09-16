#include "resolve_internal.h"
#include "name.h"
#include "resolve.h"
#include "verify.h"
#include "summary.h"
#include "analyze.h"
#include "own.h"
#include "../intrinsic/primitive.h"
#include "fold.h"
#include "select.h"
#include "../intrinsic/text.h"

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
 * `@inline(n) @capacity(n)` on a container type - Implementation-Containers.md §7, as far as this
 * stage can carry it.
 *
 * **Only the capacity is honoured here, and the placement is not.** `@capacity(n)` says the
 * container starts with room for `n` elements, which is a statement about the container and is what
 * the one caller in `lib/` depends on: `withPathCString` writes `PATH_MAX` bytes through
 * `valuesMut` into slots above the length. `@inline(n)` says those slots live in the frame, and that
 * is a *placement*, which this stage decides nowhere - `newRun` puts every run on the heap here, for
 * the reason Analysis-Manifestations.md §9.3 gives: the escape analysis that would place one in the
 * frame is unbuilt. So a refined array costs an allocation and a free that the shipping stage does
 * not pay, and means the same thing.
 *
 * The pair is checked exactly as `resolveContainerRefinement` checks it in the shipping stage,
 * because the rows that spill are the ones neither stage builds and refusing them by name is what
 * keeps an author from believing in a layout.
 *
 * Answers false having reported. `into` is left zero where nothing was written.
 */
bool Resolver::readContainerRefinement(ast::Type& type, U64& into) {
    if(!type.attributes) return true;

    Maybe<U64> slots;
    Maybe<U64> capacity;
    LocationId source = type.source;

    for(auto attribute: parse[type.attributes]->contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));
        if(which != DeclAttribute::Inline && which != DeclAttribute::Capacity) continue;

        source = attribute.source;

        if(attribute.args.size() != 1) {
            context.diagnostics.error("`@inline` and `@capacity` each take one count - "
                                      "`@inline(4) @capacity(4) [Int]`"_v, attribute.source);
            return false;
        }

        // A written number, for `readBitsAttribute`'s reason one attribute over: a bound is a
        // property of the declaration and nothing in the module has been evaluated yet.
        auto written = attribute.args.get(parse, 0).value;
        if(written.kind != ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
            context.diagnostics.error("`@inline` and `@capacity` take a written number - "
                                      "`@inline(4) @capacity(4) [Int]`"_v, written.source);
            return false;
        }

        if(which == DeclAttribute::Inline) slots = Just(U64(written.lit.i()));
        else capacity = Just(U64(written.lit.i()));
    }

    if(!slots.isJust() && !capacity.isJust()) return true;

    if(!slots.isJust() || !capacity.isJust() || slots.unwrap() != capacity.unwrap()) {
        context.diagnostics.error(
            "only `@inline(n) @capacity(n)` is built - the rows that spill need a discriminant "
            "saying whether the inline bytes hold elements or a pointer to them"_v, source);
        return false;
    }

    if(!slots.unwrap()) {
        context.diagnostics.error(
            "`@inline(0) @capacity(0)` is an array that can hold nothing - write `[T]` for one that "
            "allocates, or a larger bound"_v, source);
        return false;
    }

    into = slots.unwrap();
    return true;
}

/*
 * `@box` written on a field's type.
 *
 * Read here beside `@bits` because the two are written in the same position, and they **compose**:
 * the axes are different and neither constrains the other. `@bits` refines the *type* - it says how
 * many of a `U64`'s bits are usable, which is a question about the value - and `@box` says where the
 * field's storage for that type is. So `@box @bits(48) U64` is an ordinary field: a pointer to a
 * forty-eight-bit integer, whose target is laid out at whatever the refinement says and whose slot
 * is a word like any other box's.
 *
 * Nothing has to be done to make that work, and that is the point of reading the two separately.
 * `resolveType` strips `@bits` and refines what is left, so the field's type is already the refined
 * one by the time this runs; this walk asks only whether the storage is out of line.
 */
bool Resolver::readBoxAttribute(ast::Type& type) {
    if(!type.attributes) return false;

    auto boxed = false;

    for(auto attribute: parse[type.attributes]->contents(parse)) {
        if(findAttribute(stringView(context.findName(attribute.name))) != DeclAttribute::Box) {
            continue;
        }

        if(attribute.args.size()) {
            context.diagnostics.error("`@box` takes no arguments"_v, attribute.source);
            continue;
        }

        boxed = true;
    }

    return boxed;
}

/*
 * `@host T` on a field - Implementation-Containers.md §14's elision, and `RecordField::host`.
 *
 * The third of `@bits`/`@box`'s family: an annotation written on a field's type that changes the
 * field's physical representation and that generic code sees straight through. `@bits` narrows the
 * width, `@box` moves the storage out of line, and this one says the field is not stored at all -
 * the value the record holds already answers the property, under the field's own name.
 *
 * **This stage records it and no more.** Nothing native reads it: a native record with a `@host`
 * field lays the field out exactly as it would have without one, which is what makes the attribute
 * a JS-visible *source* fact rather than a representation this stage has to implement. What reads it
 * is the JS pipeline, later.
 *
 * Both pairings are refused, and neither refusal is tidiness: `@bits` narrows a field into a word
 * shared with its neighbours and an elided field has no word, and `@box` makes the field an address
 * and an elided field has no storage to take one of.
 */
bool Resolver::readHostAttribute(ast::Type& type) {
    if(!type.attributes) return false;

    auto elided = false;
    auto conflict = false;

    for(auto attribute: parse[type.attributes]->contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));

        if(which == DeclAttribute::Bits || which == DeclAttribute::Box) conflict = true;
        if(which != DeclAttribute::Host) continue;

        if(attribute.args.size()) {
            context.diagnostics.error("`@host` takes no arguments - the property is the field's own name"_v,
                                      attribute.source);
            continue;
        }

        elided = true;
    }

    if(elided && conflict) {
        context.diagnostics.error("`@host` cannot combine with `@bits` or `@box` - a field elided onto a host property has no storage of its own, so there is nothing for a written width to narrow and nothing for an indirection to point at"_v,
                                  type.source);
        return false;
    }

    return elided;
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
    /*
     * `@box` outside a field, which is where it means nothing - `RecordField::boxed`.
     *
     * Reported rather than ignored, because the two readings of `@box Int` in an argument position
     * are a boxed *local*, which nothing implements, and a boxed *type*, which is precisely what an
     * edge annotation exists not to be. Either way the program is asking for something it will not
     * get, and an attribute silently dropped is a representation the author believes in.
     *
     * `readingFieldType` is cleared here rather than in the caller, which is what stops the
     * permission reaching a type *argument*: `Maybe(@box Int)` would have to be a different type
     * from `Maybe(Int)` and every use site would have to say so.
     */
    auto atField = readingFieldType;
    readingFieldType = false;

    if(!atField && readBoxAttribute(type)) {
        context.diagnostics.error("`@box` can only be written on a field of a record or tuple - it is a statement about where a field's storage lives, not a type"_v,
                                  type.source);
    }

    // `@host` is the same sentence one axis over - a statement about a field rather than about a
    // type - so it is refused in the same position and for the same reason.
    if(!atField && readHostAttribute(type)) {
        context.diagnostics.error("`@host` can only be written on a field of a record or tuple - it says the host value answers this property under the field's name, not that the type is one"_v,
                                  type.source);
    }

    // Resolved *without* the attribute first, so that the refinement applies to whatever the type
    // turned out to be - an alias, a builtin name, a substituted variable - rather than needing an
    // arm per way of spelling an integer.
    if(auto bits = readBitsAttribute(type)) {
        auto plain = type;
        plain.attributes = nullptr;
        return resolveBitsType(resolveType(plain), bits.unwrap(), type.source);
    }

    /*
     * A container refinement that reached *here* is one written somewhere it means nothing.
     *
     * The two positions that honour it - the `::` of an array literal in a body and in a constant -
     * read it themselves and resolve the type without the attributes, exactly as the `@bits` arm
     * above does. So anything left is a field, a parameter or a result asking for a capacity the
     * thing that fills it will not have, and that is reported rather than dropped: an annotation
     * silently ignored is a representation its author believes in.
     */
    U64 capacity = 0;
    if(!readContainerRefinement(type, capacity)) return nullptr;

    if(capacity) {
        context.diagnostics.error(
            "`@inline(n) @capacity(n)` is honoured on the `::` of an array literal, which is what "
            "builds the container - written here it names a capacity nothing would give it"_v,
            type.source);
        return nullptr;
    }

    switch(type.kind) {
        case ast::Type::Unit:
            return program.types.unit;
        case ast::Type::Con: {
            if(auto builtin = program.types.findBuiltin(context, type.name)) {
                recordReference(context, type.source, typeSymbol(decl(), builtin), builtin);
                return builtin;
            }

            /*
             * A transparent alias, which answers with what it names - `Alias`.
             *
             * Resolved on demand rather than looked up, because an alias may name one declared below
             * it: reaching this from `resolveAliasTargets`' own walk is what makes `alias A = B` and
             * `alias B = I32` order-independent, and reaching it from an ordinary type position is
             * the same call with the answer already cached.
             */
            if(auto alias = findAlias(context, program, here(), type.name, type.source)) {
                recordReference(context, type.source, aliasSymbol(decl(), alias));

                if(isGeneric(decl(), decl()[alias]->gen)) {
                    return applyAlias(alias, nullptr, 0, type.source);
                }

                return aliasTarget(alias);
            }

            if(auto record = findRecord(context, program, here(), type.name, type.source)) {
                recordReference(context, type.source, typeSymbol(decl(), asBase<Type>(record)),
                                asBase<Type>(record));

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
            if(!roomForVariable(genContext, type.name, type.source)) return nullptr;

            auto variable = genVariable(program, genContext, type.name, type.source);
            if(variable) {
                recordReference(context, type.source, typeVarSymbol(decl(), here(), variable), variable);
                return variable;
            }

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
        /*
         * `[K: V]` - the map type, written the way its literal is.
         *
         * The declaration is `known.def`'s `map` role rather than a name, on the terms every other
         * container type here is under: what `[K: V]` means is whichever `data` claimed the role,
         * and a library that declares none reports that by name instead of resolving to something
         * else called `Map`.
         */
        case ast::Type::Map: {
            auto declared = program.roles.record(intrinsic::Known::map);
            if(!declared) {
                reportMissingRole(intrinsic::Known::map, "a map type"_v, type.source);
                return nullptr;
            }

            auto& written = type.map;
            auto key = resolveType(*parse[written.from]);
            auto value = resolveType(*parse[written.to]);
            if(!key || !value) return nullptr;

            if(!requireValueType(key, parse[written.from]->source)) return nullptr;
            if(!requireValueType(value, parse[written.to]->source)) return nullptr;

            DeclPtr<Type> arguments[2] = { key, value };
            return instantiateRecord(context, program, declared, arguments, 2, type.source);
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
        case ast::Type::Site:
            // `f.Iter`, `f.Iter(a, b)`, `f.Result` - the type a declaration publishes,
            // `resolve_sig.cpp`. Its own function because what it does is a *lookup* of a value
            // name and a mint, neither of which any other arm of this switch does.
            return resolveSiteType(type);
        case ast::Type::Iter:
            // `iter(T)` - the erased iterator object, `resolve_sig.cpp`. Its own function for the
            // same reason: what it reads is a *protocol* and what it answers is a mint.
            return resolveErasedIterType(type);
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

    /*
     * A **const parameter** of the enclosing declaration, which is the one count that is not a
     * number: what `[T *n]` inside `fn (n: Int) f()` says is decided per specialization, so the type
     * names the parameter and the substitution supplies the number.
     *
     * Asked before the constant below, and that is the shadowing rule rather than an optimization: a
     * name a declaration binds means what the declaration says whatever a module-level `let` of the
     * same name holds.
     */
    if(written.kind == ast::Expr::Var) {
        if(auto variable = genVariable(program, genContext, written.var, written.source)) {
            if(!roomForVariable(genContext, written.var, written.source)) return false;

            // A *const* parameter, which is the whole of what `n: I32` on a head declares. A type
            // parameter written here would be a count that is a type, and the position that would
            // have had to multiply by it is a long way from the one that could say so.
            if(!((VariableType*)decl()[variable])->isConst()) {
                context.diagnostics.error(
                    "%@ is a type parameter, and a count is a number - declare it `%@: Int`"_v,
                    written.source, context.findName(written.var), context.findName(written.var));
                return false;
            }

            count = variable;
            return true;
        }
    }

    /*
     * Anything else is a **constant expression** - `[U8 *(4 * 1024)]`, `[Byte *pageBytes]`.
     *
     * This position takes an expression from the parser already, which is what makes the widening
     * free: the count and a global's initializer reduce by one rule, so a number a program can
     * write as a constant is a number it can write as a length. Silent, because what it cannot
     * reduce is reported below in the words this position wants.
     */
    auto notConstant = true;
    ConstantScope constant_ { *this };
    auto constant = resolveConstant(written, nullptr, &notConstant);

    if(constant.kind == Constant::Int) {
        auto value = I64(constant.asInt());
        if(!checkFixedArrayLength(value, written.source)) return false;

        length = U64(value);
        return true;
    }

    context.diagnostics.error(
        "a fixed array's count is a constant or a const parameter, and this is neither"_v,
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
    /*
     * `e :: 'T` and `e :: &T` - a sigil in front of an ascribed type.
     *
     * `let view = Native.atomicAt(p) :: 'Atomic(U32)` is what writes it, and what the tick says
     * there is what the *expression already is*: `atomicAt` hands back a view of somebody's storage,
     * and the ascription is there to say which `a` the view is of. An ascription is a hint and a
     * widening - `doc/spec/overloading.md` - so it names a **type** and never a contract: nothing
     * here could make an owned value into a loan, and a `let` reads its own ownership off the
     * initializer the way it always does.
     *
     * So the sigils are read for the type under them and contribute nothing else. A label is refused
     * rather than dropped, because it would be naming an origin set - and an expression is not a
     * signature, so there is nothing here to publish one.
     */
    auto type = &written;

    while(type->kind == ast::Type::Shared || type->kind == ast::Type::Borrow) {
        if(type->ref.group) {
            context.diagnostics.error(
                "an origin is published by a signature, and this is an expression - write the type alone"_v,
                type->source);
            return nullptr;
        }

        type = parse[type->ref.to];
    }

    if(type != &written) return resolveAscribedType(*type, value);

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
    /*
     * A lowercase name at a **counting** position introduces a variable that counts.
     *
     * `fn (Digest(d, n)) Hmac.of(...) -> [U8 *n]` is the case, and the class head is what decides
     * it: `class Digest(d -> n: Int)` says the second position is a number, so `n` written opposite
     * it is a const parameter of *this* signature and not a type it happens to share a spelling
     * with. Without this the name arrived through `resolveType` as an ordinary type variable and
     * `checkArgumentKinds` reported the one thing the author could not have written differently -
     * there is no spelling for "a const parameter introduced by a constraint" in the constraint
     * list, and `fn (n: Int, Digest(d, n))` declares it *before* the class says what it is.
     *
     * Only where the enclosing context has not already bound the name, so an explicit
     * `fn (n: Int, Digest(d, n))` still means what it says, and only where the head's parameter
     * carries an annotation - which is the same `constType` the literal arm below reads, so a
     * number and a name at one position are decided by one fact.
     */
    if(written.kind == ast::Type::Gen && !findGenVariable(decl(), genContext, written.name)) {
        auto counted = headParameterOf(decl(), head, position);

        if(counted && ((VariableType*)decl()[counted])->constType) {
            if(!roomForVariable(genContext, written.name, written.source)) return nullptr;

            auto introduced = genVariable(program, genContext, written.name, written.source,
                                          ((VariableType*)decl()[counted])->constType);
            if(introduced) return introduced;
        }
    }

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

            /*
             * A container refinement is read off and the type resolved without it - the ascription
             * is one of the two positions that honour one. A *constant* has no run to reserve, so
             * the capacity is checked here and then means nothing: a global's array literal is
             * placed bytes, and there is no growth for a bound to be a bound of.
             */
            U64 capacity = 0;
            auto written = coerce.type;
            if(!readContainerRefinement(written, capacity)) return Constant();
            if(capacity) written.attributes = nullptr;

            auto type = resolveAscribedType(written, coerce.target);
            if(!type) return Constant();

            return resolveConstant(coerce.target, type, notConstant);
        }
        case ast::Expr::Nested:
            return resolveConstant(*parse[expr.nested], hint, notConstant);

        /*
         * `4 * 1024 * 1024`, `wordBits - 1` - arithmetic, where the instance serving the operator is
         * the compiler's own. See `resolveInfixConstant`, and `foldConstantOperator` beside it for
         * why a program's own `instance Num(Money)` is not this form.
         */
        case ast::Expr::Infix: {
            auto folded = resolveInfixConstant(expr, hint);
            return folded.isSet() ? folded : notOne();
        }

        // The same rule through the written call form - `not(mask)`, and `doc/spec/overloading.md`'s
        // "an operator is a call" from the other side.
        case ast::Expr::App: {
            auto folded = resolveCallConstant(expr, hint);
            return folded.isSet() ? folded : notOne();
        }

        /*
         * A name, which in a constant position is a global whose own initializer is a constant.
         *
         * `Analysis-Initialization.md`'s stage A calls this "a reference to an **immutable** global
         * declared earlier, purely syntactic, no analysis", and both of its adjectives are
         * load-bearing. A `let &` is *storage*: its constant is what the global starts as and a
         * statement of the startup sequence may have assigned to it since, so folding a read of one
         * answers what it held before the program ran. A global declared later has no constant yet -
         * initializers are resolved in source order - so a forward reference comes back as not this
         * form and is left to the entry sequence, which is where the use-before-init rule is.
         *
         * What it is for is the compiler's own supplied constants - `wordBits`, `pageBytes`,
         * `vectorBytes`, `Target.byteOrder` - which are exactly immutable globals with constant
         * initializers, and are useless in the positions a program wants them in without this.
         */
        case ast::Expr::Var: {
            auto global = findGlobal(context, program, here(), expr.var, expr.source);
            if(global) {
                recordReference(context, expr.source, globalSymbol(program, global),
                                decl()[global]->occurrence.type);
            }

            if(!global || decl()[global]->writable) return notOne();
            if(!decl()[global]->initial.isSet()) return notOne();

            return decl()[global]->initial;
        }

        // A field of one - `origin.x`, and every step of a record a constant holds.
        case ast::Expr::Field: {
            auto& field = *parse[expr.field];
            if(field.field.kind != ast::Expr::Var) return notOne();

            auto notThis = true;
            auto subject = resolveConstant(field.target, nullptr, &notThis);
            if(subject.kind != Constant::Aggregate) return notOne();
            if(decl()[subject.type]->kind != Type::Record) return notOne();

            auto& record = *(RecordType*)decl()[subject.type];
            auto& parts = *decl()[subject.aggregate()];
            if(parts.constructor >= record.cons.size()) return notOne();

            auto arm = record.cons.get(decl(), parts.constructor);
            for(U16 i = 0; i < arm.count && i < parts.values.size(); i++) {
                if(record.field(decl(), U16(arm.first + i)).name != field.field.var) continue;
                return parts.values.get(decl(), i);
            }

            return notOne();
        }

        /*
         * `[3, 1, 4] :: [U8 *3]` - a fixed array whose elements are all constants.
         *
         * A fixed array's elements *are* the value's storage, which is the whole reason this is a
         * constant form at all: there is no run to allocate and nothing to point at, so the bytes
         * are the elements' bytes laid end to end and `materialize` writes them the way it writes
         * any other aggregate. A growable `[T]` is an allocation and has no constant form, and the
         * hint test below is what says so - `resolveArrayLiteral` refuses an owned `[T]` where it
         * is declared, so a position that reaches here with one is not one this can reduce.
         */
        case ast::Expr::Array: {
            auto array = resolveArrayConstant(expr, hint, notConstant);
            if(!array) return notOne();

            return array.unwrap();
        }

        // `{4, 5}` - the anonymous record, whose type is its elements'. See `resolveTupleConstant`.
        case ast::Expr::Tup: {
            auto tuple = resolveTupleConstant(expr, hint);
            if(!tuple) return notOne();

            return tuple.unwrap();
        }

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
         *
         * Everything else the operator can be reaches `resolvePrefixConstant`, which is the same
         * rule the infix and call forms follow - `-(7 * 3)` is a negation of a constant, and the
         * only thing that made it not one was that the sign had nothing adjacent to fold into.
         */
        case ast::Expr::Prefix: {
            if(isNegation(context, parse, expr)) {
                return resolveLiteral(parse[expr.prefix]->on, hint, true);
            }

            auto folded = resolvePrefixConstant(expr, hint);
            return folded.isSet() ? folded : notOne();
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

    ConstantScope reading { *this };
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
 * See `PendingTextDefault`. One question, asked where the role is bound and every position is in.
 *
 * **Drains**, exactly as `resolveFieldDefaults` does, and for the reason that pass states: this is
 * called more than once. A declaration path defers a record's fields past the first call, so the
 * positions those fields queue are checked by a later one - and an entry left in the queue would be
 * reported twice.
 */
void Resolver::checkTextDefaults() {
    if(!pendingTextDefaults.size()) return;

    // Asked once for the whole program rather than once per default: the answer is the same for all
    // of them, and a library that declares no `String` is one diagnostic and not one per position
    // that wrote a string.
    auto text = textType(pendingTextDefaults[0].source);
    if(!text) return;

    for(auto& pending: pendingTextDefaults) {
        if(sameType(pending.type, text)) continue;

        context.diagnostics.error("this default is %@ and the position is %@"_v, pending.source,
                                  typeText(context, decl(), text),
                                  typeText(context, decl(), pending.type));
    }

    pendingTextDefaults.clear();
}

/*
 * The record a constructor written in a constant position builds - the declaration, or the
 * instantiation the **position** asked for.
 *
 * A generic `data Opt(a) = None | Some(a)` has no type for `None` until something binds `a`, and
 * what would bind it from the *inside* is the inference a construction runs over its field values -
 * a `TypeArguments` walk this position has no builder to make one with. What binds it from the
 * outside is the position, which is written down wherever a constant is what a position is for:
 * `state: State(Int) = On` and `limit: Maybe(Int) = Just(4) :: Maybe(Int)` are the two shapes, and
 * `Types::instantiate` has already substituted that head's fields, so the walk below reads them
 * without substituting anything itself.
 *
 * Null where the position says nothing or names a different declaration - "not this form", so the
 * caller reports at the expression the author wrote rather than about a head this walk inferred.
 */
Maybe<DeclPtr<Type>> Resolver::constantHeadFor(DeclPtr<RecordType> declaration, DeclPtr<Type> hint) {
    if(!decl()[declaration]->gen) return Just(asBase<Type>(declaration));
    if(!hint || decl()[hint]->kind != Type::Record) return Nothing();

    auto& instantiated = *(RecordType*)decl()[hint];
    if(instantiated.instanceOf != declaration) return Nothing();

    return Just(hint);
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

    recordReference(context, written.type.source, constructorSymbol(decl(), found));

    auto& declared = *decl()[found.record];
    if(!reachesInside(declared, expr.source, "its constructor"_v)) return Just(Constant());

    auto type = asBase<Type>(found.record);
    auto instantiated = constantHeadFor(found.record, hint);
    if(!instantiated) return Nothing();

    auto& record = *(RecordType*)decl()[instantiated.unwrap()];
    if(record.layout != RecordLayout::Enum) return Nothing();

    type = instantiated.unwrap();

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
 * The operation a written operator or a class-member call is, where the instance serving it **at
 * these operands** is the compiler's own.
 *
 * Selection first and the fold second, which is the order that keeps this from being a second
 * reading of what `*` means: a candidate is a class member, the instance is looked up the way every
 * call looks one up, and only a *generated* instance has an operation at all. A program's own
 * `instance Num(Money)` therefore does not fold here - it is a body, and nothing in this stage
 * runs a body at compile time. `Analysis-Initialization.md`'s stage A is where that is left open.
 *
 * Over the whole set rather than over `sole()`, because `+` is genuinely overloaded in this library:
 * `Num.+` and `lib/Native`'s pointer offset share the name, and which one this is is decided by
 * which class has an instance at the operand's type.
 */
bool Resolver::constantOperation(const OverloadSet& set, DeclPtr<Type> operand, LocationId source,
                                 BinaryOp& binary, UnaryOp& unary, bool& isBinary) {
    for(Size i = 0; i < set.size(); i++) {
        auto& candidate = *decl()[set.at(i)];
        if(!candidate.klass) continue;

        InstanceMatch found;
        if(!selectInstance(context, program, candidate.klass, &operand, 1, source, false, found)) {
            continue;
        }

        auto& instance = *decl()[found.instance];
        if(candidate.classMember >= instance.members.size()) continue;

        auto member = instance.members.get(decl(), candidate.classMember);
        if(!member) continue;

        auto& provided = *decl()[member];
        if(provided.provided == intrinsic::Provided::Count) continue;

        if(providedOperation(provided.provided, provided.providedMember, binary, unary, isBinary)) {
            return true;
        }
    }

    return false;
}

/*
 * One application of such an operator over constants - the answer, or nothing.
 *
 * A **refinement is refused** rather than folded, and that is `doc/spec/repr.md`'s rule rather than
 * a limit here: a load widens and a store narrows, so arithmetic never operates on a narrowed value
 * at all - `addSmall(12)` on a `@bits(4) U32` is 24 because the addition is the `U32` one. Folding
 * at the refinement's width would be the one answer this tier could give that the machine does not.
 */
Constant Resolver::foldConstantOperator(const OverloadSet& set, const Constant* args, Size count,
                                        LocationId source) {
    if(!count || count > 2) return Constant();
    if(!args[0].isSet() || !args[0].type) return Constant();
    if(isRefinedInt(decl(), args[0].type)) return Constant();

    BinaryOp binary;
    UnaryOp unary;
    bool isBinary;

    if(!constantOperation(set, args[0].type, source, binary, unary, isBinary)) return Constant();
    if(isBinary != (count == 2)) return Constant();

    if(!isBinary) return foldConstantUnary(program, unary, args[0], args[0].type);

    // A comparison answers `Bool` and everything else answers the operands' own type, which is what
    // `provided.def`'s heads say and is the one thing the folder cannot read off its inputs.
    auto category = binaryCategory(binary);
    auto result = category == OpCategory::Equality || category == OpCategory::Order
                      ? booleanType(program) : args[0].type;

    return foldConstantBinary(program, binary, args[0], args[1], result);
}

/*
 * `a + b` in a constant position - the chain flattened and climbed exactly as a value would be.
 *
 * Precedence climbing rather than the written tree, for `climb`'s reason: the parser nests to the
 * right whatever the fixity, so `1 + 2 * 3` arrives as `1 + (2 * 3)` only by accident of these
 * particular operators. Sharing `flattenChain` is what keeps the grouping one answer.
 *
 * The **hint travels down only where it is a number**. A comparison's position asks for `Bool` and
 * its operands are not booleans, so handing that hint to them would decide the wrong type; a numeric
 * position is the case where the hint is what settles `4 * 1024`'s width. The right operand then
 * takes the left's type, which is `climb`'s rule with the parameter type replaced by the value it
 * would have been.
 */
Constant Resolver::resolveInfixConstant(ast::Expr& expr, DeclPtr<Type> hint) {
    Chain chain;
    if(!flattenChain(expr, chain, false)) return Constant();

    Size operandIndex = 0;
    Size operatorIndex = 0;

    auto numeric = hint && (decl()[hint]->kind == Type::Int || decl()[hint]->kind == Type::Float);
    return climbConstant(chain, operandIndex, operatorIndex, 0, numeric ? hint : nullptr);
}

Constant Resolver::climbConstant(Chain& chain, Size& operandIndex, Size& operatorIndex,
                                 U8 minimumPrecedence, DeclPtr<Type> hint) {
    auto notConstant = true;
    auto lhs = resolveConstant(*chain.operands[operandIndex++], hint, &notConstant);
    if(!lhs.isSet()) return Constant();

    while(operatorIndex < chain.operators.size() &&
          chain.operators[operatorIndex].precedence >= minimumPrecedence) {
        auto op = chain.operators[operatorIndex];
        auto source = chain.sources[operatorIndex];
        operatorIndex++;

        // `&&` and `||` are control flow rather than operations - `op.def`'s `Flow` roles - so there
        // is nothing here to fold them into and they are simply not this form.
        if(op.form != OperatorForm::Call) return Constant();

        auto next = U8(op.assoc == Assoc::Right ? op.precedence : op.precedence + 1);
        auto rhs = climbConstant(chain, operandIndex, operatorIndex, next, lhs.type);
        if(!rhs.isSet()) return Constant();

        Constant operands[2] = { lhs, rhs };
        lhs = foldConstantOperator(op.set, operands, 2, source);
        if(!lhs.isSet()) return Constant();
    }

    return lhs;
}

/*
 * `not(mask)`, `min(a, b)` - a call in a constant position, folded where its callee is one of the
 * compiler's own instance members.
 *
 * The same rule as the operator above, reached through the written call form: `doc/spec/overloading.md`
 * is explicit that an operator *is* a call, so a position that folded one and not the other would be
 * making the spelling matter.
 */
Constant Resolver::resolveCallConstant(ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.app];
    if(written.callee.kind != ast::Expr::Var) return Constant();
    if(written.args.size() < 1 || written.args.size() > 2) return Constant();

    OverloadSet set;
    findOverloads(context, program, here(), written.callee.var, written.args.size(),
                  expr.source, set);

    if(set.isEmpty()) return Constant();

    SmallArray<Constant, 2> values;
    auto numeric = hint && (decl()[hint]->kind == Type::Int || decl()[hint]->kind == Type::Float);

    for(auto argument: written.args.contents(parse)) {
        // A named argument decides which position it fills, which is a mapping this path has no
        // reason to repeat: a constant call of two arguments is spelled positionally or it is not
        // this form.
        if(argument.name != StringId()) return Constant();

        auto notConstant = true;
        auto value = resolveConstant(argument.value, values.size() ? values[0].type
                                                                  : (numeric ? hint : nullptr),
                                     &notConstant);
        if(!value.isSet()) return Constant();

        values.push(value);
    }

    return foldConstantOperator(set, values.pointer(), values.size(), expr.source);
}

/*
 * `-(7 * 3)`, `-limit` - a prefix operator over a constant, once the sign has failed to fold into a
 * literal.
 *
 * The arm above this catches `-1`, which is a *written* negative number rather than an operation on
 * a positive one, and it catches only that: `resolvePrefix` folds the sign into whatever is
 * syntactically adjacent, and a parenthesis or a name is not. What was left over then reached no
 * constant form at all - so a global written `-(21)` became a dynamic initializer while `-21` was a
 * constant, which is a difference in the parentheses and in nothing a program can see.
 *
 * `resolveCallConstant`'s body at one operand and through the prefix's own spelling, on
 * `doc/spec/overloading.md`'s "an operator is a call": the operand folds first, the name reaches its
 * set at arity one exactly as `resolvePrefix` finds it, and `foldConstantOperator` decides whether
 * the instance serving it there is one of the compiler's. `!` is `op.def`'s `truthNot` and has no
 * class, so it declines there rather than needing a test here.
 */
Constant Resolver::resolvePrefixConstant(ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.prefix];
    if(written.op.kind != ast::Expr::Var) return Constant();

    // The hint travels down only where it is a number, which is `resolveInfixConstant`'s rule and
    // is what settles `-(4 * 1024) :: I32`'s width at the operand rather than after it.
    auto numeric = hint && (decl()[hint]->kind == Type::Int || decl()[hint]->kind == Type::Float);

    auto notConstant = true;
    auto value = resolveConstant(written.on, numeric ? hint : nullptr, &notConstant);
    if(!value.isSet()) return Constant();

    OverloadSet set;
    findOverloads(context, program, here(), written.op.var, 1, written.op.source, set);
    if(set.isEmpty()) return Constant();

    return foldConstantOperator(set, &value, 1, expr.source);
}

/*
 * `[1, 2, 3]` in a constant position - the aggregate the elements are, or "not this form".
 *
 * `resolveAggregateConstant`'s shape one type down, and the same three answers for the same
 * reasons: `Nothing` is not this form and reports nothing, `Just` of an unset constant is this form
 * gone wrong and has reported, and `Just` of a value is the constant. A hint that is not a fixed
 * array is the first of those rather than a message, so `[1, 2, 3]` with no ascription is reported
 * once - by the value path, which names the ascription that would fix it.
 *
 * The literal's own rules are `arrayLiteralShape`'s and are not restated here. What is left is the
 * walk, and it is the field walk's: an element that is not a constant makes the whole literal not
 * this form, so a root module's `let xs = [n, 2, 3] :: [Int *3]` is still the runtime array it has
 * to be, and an element that *is* a constant of the wrong type reports at the element.
 */
Maybe<Constant> Resolver::resolveArrayConstant(ast::Expr& expr, DeclPtr<Type> hint,
                                               bool* notConstant) {
    if(!hint || decl()[hint]->kind != Type::Array) return Nothing();

    auto written = expr.arr.contents(parse);
    auto shaped = arrayLiteralShape(expr, hint, written.size());
    if(!shaped) return Just(Constant());

    // A count only a specialization knows is not a constant *here*: an `AggregateConstant` holds one
    // value per element and there is no number to make that list as long as. "Not this form", so the
    // value path builds the fill - which is where a quantified count can be one at all, since a
    // constant position is never inside a generic body.
    if(shaped.unwrap().quantified) return Nothing();

    auto element = shaped.unwrap().element;

    SmallArray<Constant, 8> values;
    auto ok = true;
    auto reduced = true;

    for(Size i = 0; i < written.size(); i++) {
        auto& item = *written.pointerAt(i);

        auto dynamic = false;
        auto value = resolveConstant(item, element, &dynamic);

        if(dynamic) { reduced = false; continue; }
        if(!value.isSet()) { ok = false; continue; }

        if(!sameType(value.type, element)) {
            context.diagnostics.error("this element is %@ and %@ holds %@"_v, item.source,
                                      typeText(context, decl(), value.type),
                                      typeText(context, decl(), hint),
                                      typeText(context, decl(), element));
            ok = false;
            continue;
        }

        values.push(value);
    }

    // "Not this form" before "this form is wrong", which is `resolveAggregateConstant`'s order and
    // is what keeps a literal one of whose elements is a runtime value compiling: the value path
    // builds it, and a report made here would be a report that path never needed.
    if(!reduced) {
        if(notConstant) *notConstant = true;
        return Nothing();
    }

    if(!ok) return Just(Constant());

    // The fill, spread here rather than by the constant: what an `AggregateConstant` holds is one
    // value per element, for `addAggregate`'s reason one tier up.
    while(values.size() < shaped.unwrap().length) values.push(values[0]);

    return Just(program.aggregateConstant(hint, 0, values.pointer(), values.size()));
}

/*
 * `{0, True, 0.5}` - the anonymous record as a constant. J61.
 *
 * `resolveTupleLiteral` with `resolveConstant` where it has `resolveValue`, which is the same
 * difference between a construction that runs and one that reduces that `resolveAggregateConstant`
 * is on the other side of. What made it a separate function rather than a case of that one is the
 * thing that makes a tuple a tuple: **there is no declaration**. A `data` is what a construction is
 * checked against, and an anonymous record's type is interned from what its elements turned out to
 * be - so the fields are built here after the walk rather than read from an arm before it.
 *
 * The hint decides the elements, on `resolveTupleLiteral`'s rule and with its test: the arity **and
 * the labels** are the type's identity, so a hint differing in either is a hint about another type
 * and pushing its field types down would be filling positions that are not these.
 *
 * A field whose constant comes out at another type is **not this form** rather than a mistake, and
 * that is `resolveAggregateConstant`'s ordering restated where it has no declaration to compare
 * against: the value path widens a field this one only reads, so `{n, 2 :: I8}` at a `{I32, I32}`
 * position is a construction that runs and not a constant that is wrong.
 *
 * Why it was missing: `resolveConstant` had no `Tup` arm at all, so every tuple was "not a constant
 * expression" - which cost a global initialized with one, a field or parameter default that is one,
 * and, because `matchAsMap` evaluates its arms through here, **every constructor map whose result is
 * a tuple**. `axesOf` in `test/resolve/ConstructorMap.Records.yana` is the fixture that had been
 * asserting the branch chain; findings.md §106.5 is where it was counted.
 */
Maybe<Constant> Resolver::resolveTupleConstant(ast::Expr& expr, DeclPtr<Type> hint) {
    auto written = expr.tup;
    auto count = U32(written.size());

    // `{}` is the unit rather than a record of nothing - see the `Tup` arm of `resolveExpression`,
    // where the same split is made for the same reason.
    if(!count) return Just(unitConstant(program.types.unit));

    auto expected = (RecordType*)nullptr;

    if(hint && decl()[hint]->kind == Type::Record) {
        auto record = (RecordType*)decl()[hint];

        if(record->tuple && record->fields.size() == count) {
            expected = record;

            auto index = U16(0);
            for(auto argument: written.contents(parse)) {
                if(record->field(decl(), index++).name != argument.name) expected = nullptr;
            }
        }
    }

    SmallArray<Constant, 8> values;
    auto index = U32(0);

    for(auto argument: written.contents(parse)) {
        auto declared = expected ? expected->field(decl(), U16(index)).occurrence.type : nullptr;
        index++;

        auto dynamic = false;
        auto value = resolveConstant(argument.value, declared, &dynamic);

        if(dynamic) return Nothing();
        if(!value.isSet()) return Just(Constant());
        if(declared && !sameType(value.type, declared)) return Nothing();

        values.push(value);
    }

    // The type is the values', which is the whole of what an anonymous record's identity is - the
    // written labels carried through beside the types the elements came out at.
    SmallArray<RecordField, 4> fields;
    index = 0;

    for(auto argument: written.contents(parse)) {
        RecordField row;
        row.name = argument.name;
        row.occurrence = owned(values[index++].type);
        row.source = argument.value.source;
        fields.push(row);
    }

    // The same rule the written form is under, asked in the same words - `uniqueTupleLabels`.
    if(!uniqueTupleLabels(fields.pointer(), fields.size())) return Just(Constant());

    auto type = tupleType(context, program, fields.pointer(), fields.size(), expr.source);
    if(!type) return Just(Constant());

    // The one constructor an anonymous record has - see `tupleType`, where it is made.
    return Just(program.aggregateConstant(type, 0, values.pointer(), values.size()));
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

    recordReference(context, written.type.source, constructorSymbol(decl(), found));

    auto& declared = *decl()[found.record];
    if(!reachesInside(declared, expr.source, "its constructor"_v)) return Just(Constant());

    auto instantiated = constantHeadFor(found.record, hint);
    if(!instantiated) return Nothing();

    auto& record = *(RecordType*)decl()[instantiated.unwrap()];
    auto type = instantiated.unwrap();

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
 * `"abc"` - the kind test, said once because both readers of a literal ask it.
 *
 * `resolveLiteral` and `literalValue` are the same question at two altitudes - what a literal *is*
 * and what node builds it - and a string is the one kind that answers ahead of `writtenLiteral` in
 * both. Two spellings of `expr.kind - ast::Expr::Lit` is how one of them comes to test the wrong
 * kind after an enum grows an entry.
 */
bool isStringLiteral(ast::Expr& expr) {
    return ast::isLiteral(expr) &&
           ast::Literal::Kind(expr.kind - ast::Expr::Lit) == ast::Literal::String;
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

    /*
     * A string, which is a constant the way truth is: there is nothing to select and nothing to
     * fold. `message: String = ""` is what this admits and what it is for.
     *
     * The type is the **position's**, and the check that the position is one a string can fill is
     * deferred - `PendingTextDefault`, where the ordering that forces it is argued. Where there is
     * no position the role answers, which is every caller that runs after the declarations.
     */
    if(isStringLiteral(expr)) {
        if(negative) {
            context.diagnostics.error("a string cannot be negated"_v, expr.source);
            return Constant();
        }

        if(!hint) {
            auto type = textType(expr.source);
            return type ? textConstant(type, expr.lit.s) : Constant();
        }

        /*
         * `separator: CodeUnit = ","` - a literal at a type it converts to, which in a constant
         * position has to be one that folds. A declared `fromText` is a call with a body, refused
         * in the words the number's refusal below uses and for its reason.
         */
        switch(auto target = textTarget(hint)) {
            case TextTarget::Integer:
            case TextTarget::Character:
                return textCharConstant(expr.lit.s, hint, target, expr.source);
            case TextTarget::Conversion:
                context.diagnostics.error(
                    "a literal of %@ is a call of its %@ instance rather than a constant, and this position needs a constant"_v,
                    expr.source, typeText(context, decl(), hint),
                    context.findName(decl()[program.roles.klass(intrinsic::Known::fromText)]->name));
                return Constant();
            case TextTarget::Text:
                break;
        }

        pendingTextDefaults.push(PendingTextDefault { hint, expr.source });
        return textConstant(hint, expr.lit.s);
    }

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

    /*
     * `let &sink = 0 :: Vec(I32, 4)` - a literal at a **vector**, which is the same number in every
     * lane and therefore an aggregate of `lanes` constants.
     *
     * The splat is what `FromInt`'s vector instance already computes in a value position
     * (`expand_provided.cpp`, where the argument for that answer is made), said here as a value
     * rather than as a node: a constant position has no builder, and the bytes a global holds before
     * anything runs are exactly the lane's bytes repeated. So the two paths agree by construction
     * and neither is a second reading of what a number at a vector means.
     *
     * A count nothing has decided - the natural form inside a generic body, or a quantified one - is
     * refused by the ordinary message below rather than here: there is no number to make the list as
     * long as, which is `resolveArrayConstant`'s refusal at a different shape.
     */
    if(auto lanes = vectorLanes(decl(), type)) {
        auto lane = resolveLiteral(expr, laneTypeOf(decl(), type), negative);
        if(!lane.isSet()) return Constant();

        SmallArray<Constant, 8> values;
        for(U32 i = 0; i < lanes; i++) values.push(lane);

        return program.aggregateConstant(type, 0, values.pointer(), values.size());
    }

    /*
     * Asked of the **canonical** integer, which is `canonicalInt`'s rule and not an exception here:
     * `@bits(2) U32` is a type of its own because its storage differs, and what it dispatches,
     * computes and converts as is the `U32` it refines. So `0 :: HeapFlag` is the compiler's own
     * `FromInt(U32)` folded into the refinement's normal form - `foldWritten` reads the width off
     * the refinement, so the number that comes out is the narrow one - rather than a call nobody
     * declared, which is what asking the family about the refinement itself reported.
     */
    if(!providedFor(program, providerOf(written.role), canonicalInt(decl(), type))) {
        context.diagnostics.error(
            "a literal of %@ is a call of its %@ instance rather than a constant, and this position needs a constant"_v,
            expr.source, typeText(context, decl(), type),
            context.findName(decl()[program.roles.klass(written.role)]->name));
        return Constant();
    }

    return foldWritten(written, type, expr.source);
}

/*
 * The type a string literal has - the record `known.def`'s `string` row names.
 *
 * Read off the role rather than named here, on `type.def`'s terms: `String` is an ordinary record
 * of `lib/Core` and this stage has no row for it, so what a literal *is* is the library's answer
 * and this is the one question asked to get it.
 *
 * The **record** row and no longer `stringLiteral`'s result type, which is J46 §3.5's first line:
 * asking a function what a string is meant the library's builder had to exist for the grammar to
 * have a type at all, and the row that says which record is a string has been sitting beside it
 * since `StringType::content` was filled from it.
 */
DeclPtr<Type> Resolver::textType(LocationId source) {
    auto declared = program.roles.record(intrinsic::Known::string);
    if(!declared) {
        reportMissingRole(intrinsic::Known::string, "a string literal"_v, source);
        return nullptr;
    }

    return DeclPtr<Type>(declared.offset);
}

/*
 * See the declaration. Asked of every string literal that has a position, so the common answer is
 * the cheap one: a `String` position is `Text` on one comparison and never reaches a selection.
 *
 * **The compiler's own targets are recognized by kind and by role, not by instance.** A field
 * default is resolved before the classes have their members, so asking `FromText` there would answer
 * "no instance" for `CodeUnit` in exactly the position where `separator: CodeUnit = ","` needs a
 * constant - and the answer does not depend on it: an integer or a `Char` literal is a constant
 * whatever the library says its run-time conversion is (`lib/Core/Char.yana`, where that is stated).
 * A refinement is asked through, on `canonicalInt`'s terms, and `foldLiteral` narrows the constant
 * to the refinement's normal form.
 *
 * Anything else converts only where the class has an instance to call - the enclosing signature's
 * requirement at a variable, or a declared instance at a concrete type - and is otherwise `Text`,
 * which leaves the ordinary conversion to report `String` against the position as it always did.
 */
Resolver::TextTarget Resolver::textTarget(DeclPtr<Type> type) {
    if(!type) return TextTarget::Text;

    auto text = program.roles.record(intrinsic::Known::string);
    if(text && type.offset == text.offset) return TextTarget::Text;

    if(decl()[canonicalInt(decl(), type)]->kind == Type::Int) return TextTarget::Integer;

    auto character = program.roles.record(intrinsic::Known::character);
    if(character && type.offset == character.offset) return TextTarget::Character;

    auto klass = program.roles.klass(intrinsic::Known::fromText);
    if(!klass || !decl()[klass]->members.size()) return TextTarget::Text;

    if(!isConcreteType(decl(), type)) {
        if(decl()[type]->kind != Type::Variable) return TextTarget::Text;

        auto enclosing = currentFunction ? currentFunction->gen : nullptr;
        return satisfiesRequirement(context, program, enclosing, klass, &type, 1)
                   ? TextTarget::Conversion : TextTarget::Text;
    }

    InstanceMatch found;
    return selectInstance(context, program, klass, &type, 1, kNullLocation, false, found)
               ? TextTarget::Conversion : TextTarget::Text;
}

/*
 * See the declaration - part 1.1's acceptance table, and the fold. The table and its words are
 * `intrinsic/text.h`'s, shared with the shipping stage so that the two accept the same literals.
 *
 * What is refused is reported at the literal and answers unset, so a position is not also told that
 * a `String` does not fit it.
 */
Constant Resolver::textCharConstant(StringId written, DeclPtr<Type> type, TextTarget target,
                                    LocationId source) {
    auto value = textCodePoint(written, type, target, source);
    if(!value.isSet() || target != TextTarget::Character) return value;

    return program.aggregateConstant(type, 0, &value, 1);
}

/*
 * The checked code point itself - `textCharConstant` without the record around a `Char`, which is
 * what a value position builds as a construction instead.
 */
Constant Resolver::textCodePoint(StringId written, DeclPtr<Type> type, TextTarget target,
                                 LocationId source) {
    auto text = context.findName(written);
    auto chars = intrinsic::literalChars(text.text(), text.size());

    intrinsic::Numeric integer;
    auto isInteger = target == TextTarget::Integer;
    if(isInteger) integer = ((IntType*)decl()[canonicalInt(decl(), type)])->numeric();

    auto refusal = intrinsic::checkLiteralChar(chars, isInteger ? &integer : nullptr);
    if(refusal != intrinsic::CharRefusal::None) {
        intrinsic::reportCharRefusal(refusal, chars, typeText(context, decl(), type),
                                     [&](auto format, auto&&... arguments) {
            context.diagnostics.error(format, source, arguments...);
        });

        return Constant();
    }

    if(target == TextTarget::Integer) return foldLiteral(decl(), type, chars.first, double(chars.first));

    // The one field of a `Char` - the `U32` the declaration says it is.
    auto& record = *(RecordType*)decl()[type];
    auto arm = record.cons.get(decl(), 0);
    return intConstant(record.field(decl(), arm.first).occurrence.type, chars.first);
}

/*
 * See the declaration. The constant where the target is the compiler's, and the call where it is not
 * - `literalNode`'s two answers for a number, with the text as the argument.
 *
 * A `Char` is built as the construction it is rather than placed as an aggregate constant: a record
 * in a value position is storage, and `resolveAggregateConstant` is where why this stage folds none
 * is written down. The field is still the constant, so what a dump shows is one `const` and the
 * aggregate over it.
 */
Node* Resolver::textConversionValue(Builder& builder, StringId written, DeclPtr<Type> type,
                                    TextTarget target, LocationId source) {
    if(target != TextTarget::Conversion) {
        auto value = textCodePoint(written, type, target, source);
        if(!value.isSet()) return nullptr;

        if(target == TextTarget::Integer) return builder.addConst(source, value);

        auto component = builder.addConst(source, value);
        return builder.addAggregate(source, type, 0, &component, 1);
    }

    auto& callee = *decl()[decl()[program.roles.klass(intrinsic::Known::fromText)]->members.get(decl(), 0)];
    auto& parameter = *decl()[callee.args.get(decl(), 0)];

    TypeArguments typeArguments(*this, callee.gen);
    typeArguments.bind(callee.result.type, type);

    auto argument = textValue(builder, written, source);
    if(!argument) return nullptr;

    auto prepared = prepareArgument(builder, *argument, parameter, typeArguments, source);
    if(!prepared) return nullptr;

    SmallArray<Node*, 8> arguments;
    arguments.push(prepared);
    return finishCall(builder, callee, arguments, typeArguments, nullptr, source);
}

/*
 * The **bytes** of a string literal, as a constant global of `[CodeUnit *n]`.
 *
 * One of the two things about a literal that are the compiler's, and the one that is the same
 * whatever is done with the value: the bytes are laid out by the array's own manifestation and the
 * global outlives the program, so an address of it is a pointer nothing has to keep alive.
 *
 * A global per literal rather than one interned by content - the name would then have to be derived
 * from bytes that may hold a quote or a newline, which is a decision worth making once and later
 * rather than as a side effect of emitting the first one. `resolve/const.cpp`'s
 * `stringLiteralBytes` is the shipping stage's, and it records the same choice.
 */
DeclPtr<Global> Resolver::stringBytesGlobal(StringId written, LocationId source) {
    auto text = context.findName(written);
    auto count = U64(text.size());

    auto unit = program.types.codeUnit;
    auto bytes = program.types.arrayType(decl(), unit, count);

    /*
     * The initializer is the **text itself**, at the array type - not `count` interned constants.
     *
     * `Constant::Text` says what the value is and `materialize` writes it, which is one entry in
     * the aggregate table per literal instead of `count` of them plus the list over them. What that
     * cost while it was a list is `test/bench/findings.md` §94: interning is structural, so every
     * literal walked every byte of every literal before it.
     */
    auto global = new (program.decl) Global(program.uniqueSymbol(here(), context, "string"_v),
                                            owned(bytes));
    global->source = source;
    global->initial = textConstant(bytes, written);

    auto pointer = global - *program.decl;
    decl()[here()]->globals.push(program.decl, pointer);
    return pointer;
}

/*
 * `"abc"` - the constant, in a value position.
 *
 * **One arm now, where there were two.** This used to try `stringLiteral`'s body as a constant and
 * fall back to *emitting the call*, opened by `@inline(always)`, for a target whose string has no
 * static form. There is no such target in this stage - `-resolver ownership` is native only - and
 * the fallback was the expensive half of a decision that had one answer: five bodies to resolve,
 * inline and fold at every literal in the program, to arrive at three field writes of numbers the
 * compiler already had.
 *
 * So a literal is a constant here and in a constant position alike, built by `stringConstant`
 * out of the layout, and `Lowering::placeConstant` puts it in read-only data once per distinct
 * literal. The day a target's string has no static form is the day this grows a second arm again,
 * and it will be *this* function that grows it rather than each of the four sites that spell a
 * literal.
 */
Node* Resolver::textValue(Builder& builder, StringId written, LocationId source) {
    auto value = stringConstant(written, source);
    if(!value.isSet()) return nullptr;

    return builder.addConst(source, value);
}

/*
 * A constant in a value position - the node, whatever kind it is.
 *
 * One function rather than `addConst` at each of the four sites that materialize an authored
 * constant, because `Constant::Text` is not an immediate: a string is a run of bytes somewhere and a
 * value built out of it, so a default argument, a field default and a `@lazy` thunk all have to
 * reach `textValue` and would each have grown the same branch.
 */
Node* Resolver::constantValue(Builder& builder, const Constant& constant, LocationId source) {
    if(constant.kind == Constant::Text) return textValue(builder, constant.asText(), source);

    return builder.addConst(source, constant);
}

Node* Resolver::literalValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool negative) {
    if(auto truth = truthLiteral(expr); !negative && truth.isSet()) {
        return builder.addConst(expr.source, truth);
    }

    if(isStringLiteral(expr)) {
        if(negative) {
            context.diagnostics.error("a string cannot be negated"_v, expr.source);
            return nullptr;
        }

        if(auto target = textTarget(hint); target != TextTarget::Text) {
            return textConversionValue(builder, expr.lit.s, hint, target, expr.source);
        }

        return textValue(builder, expr.lit.s, expr.source);
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

void Resolver::bindLocal(StringId name, Local* local) {
    bindings.push(Binding { name, local });
    if(!recording(context) || !local || !currentFunction || name == StringId()) return;

    // As an occurrence of itself as well as a definition: a symbol says which binding a name is and
    // a reference says what type it had, and the declaration is where the type is known.
    auto symbol = localSymbol(decl(), *program.mir, currentFunction - *program.decl, *local);
    recordDefinition(context, symbol);
    recordReference(context, local->source, symbol, local->occurrence.type);
}

void Resolver::recordLocal(LocationId source, Local& local) {
    if(!recording(context) || !currentFunction) return;
    recordReference(context, source, localSymbol(decl(), *program.mir, currentFunction - *program.decl, local),
                    local.occurrence.type);
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
Ptr<Program> resolveProgram(Context& context, ast::ModuleGroup& root, ast::ModuleProvider* provider,
                            Buffer<ast::ModuleGroup*> testRoots) {
    // The one invariant `compiler/intrinsic`'s four tables cannot state as a `static_assert`: no two
    // rows anywhere share a role name. A duplicate would make which of them `@builtin` reached
    // depend on the order the tables are searched, silently. Assertion builds only.
    intrinsic::verifyRoleTable();

    auto program = Ptr<Program>(new Program());

    Array<ast::ModuleGroup*> groups;
    discoverModules(context, *program, root, provider, groups, testRoots);

    Resolver resolver { context, *program, *context.parseRegion, nullptr, toBuffer(groups) };

    /*
     * And the one thing the resolver leaves behind it for the walks below - §6.4's witness entry.
     *
     * A witness is filled by the reachability walk, at argument lists only that walk discovers, and
     * the "make an object" entry holds a function over the *instance's* own state record - which is
     * minted from that instance's body and is therefore the resolver's to make. See
     * `Program::IterObjects`.
     */
    resolver.objectHook.resolver = &resolver;
    program->iterObjects = &resolver.objectHook;

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
     * The passes between the declarations and the bodies, wrapped so that a program which wrote a
     * **declaration path** can run them before the drain below - and so that one which did not runs
     * them exactly where it always did.
     *
     * The drain demands a *body*, and the order above says what a body needs: the third point, every
     * type classified, and the fourth, every global holding the type its initializer gave it.
     * Reading one earlier than that is silent rather than loud - a read of an untyped global answers
     * null and takes its whole statement with it, and an unclassified record reads as trivially
     * copyable - which is `../../doc/progress/2026-09-16-J57-object-defects.md` §1, found by writing
     * a lexer as an `iter fn`. So the passes move to the front of the queue for the programs that
     * need them, and nothing moves for the programs that do not.
     *
     * Three groups rather than one, because the checkpoint below sits between them and a program
     * with no path must see it in the same place. Each runs once, whichever caller reaches it first -
     * with the exception the first of them is split around, immediately below.
     */

    /*
     * The three queues a declaration walk fills, drained - and the one group here that is **not**
     * latched, because it is the one that has more than one walk to drain.
     *
     * `resolveFieldDefaults` is why. A `data` naming a declaration path has its fields deferred -
     * `deferFields` - so the defaults on the *rest* of that record's fields are queued by
     * `resolveSiteSignatures`, which runs after the group below has already latched. Draining once
     * left them in the queue and `Holder {it: upTo.Iter, size: Int = 4 * 1024}` was "Holder leaves
     * out size, which has no default" at every construction of it. The other two are here because
     * they are queued by the same walk and read the same way, and a string default *of a field* is
     * queued by the first of the three.
     *
     * Safe to call twice because all three clear what they drained, which is the property this
     * relies on and which each of them states.
     */
    auto drainDeclarationQueues = [&]() {
        /*
         * The field defaults the record walk queued - `Resolver::PendingDefault`, where the ordering
         * this closes is argued.
         *
         * Here rather than beside the fields for **two** reasons now. Every constructor in the
         * program exists, which is the original one: `{f: Flag = Off}` above `Flag` names a
         * constructor table still being built. And every *instance* does, including the ones the
         * compiler generates - a default is a constant expression, and `4 * 1024` is a constant only
         * where the `Num(I32)` serving the `*` can be looked up and found to be the compiler's own.
         * A parameter default is resolved inside `declare` and had both already; a field default was
         * two passes early and silently had neither.
         *
         * Before `checkTextDefaults` below, which is what a *string* default of a field is queued
         * for.
         */
        resolver.resolveFieldDefaults();

        // The string defaults every pass above queued, now that `String`'s own declaration is
        // certainly in - `Resolver::PendingTextDefault`, where the ordering this closes is argued.
        resolver.checkTextDefaults();

        // And the `@caller` positions that have to be that same type - `Resolver::PendingCallerText`.
        resolver.checkCallerTexts();
    };

    auto declared = false;
    auto settleDeclarations = [&]() {
        if(declared) return;
        declared = true;

        /*
         * The `Iterate` instance of every erased `iter(T)` a declaration wrote - the same pass
         * problem from the other side.
         *
         * A written `iter(T)` is minted where it is read, which for a field is before `lib/Core`'s
         * `Iterate` is certainly a declaration; the *type* needs nothing but the protocol, so what
         * waits is the instance that steps it. See `completeErasedIterates`.
         */
        resolver.completeErasedIterates();

        /*
         * And the instances whose head is a **payload-free sum** - `provideEnumInstances`, one
         * `Enum`, `Eq` and `Ord` per `data`.
         *
         * Here rather than beside `provideInstances` above, and the position is the whole difference
         * between the two: the families up there are the target's widths, which no declaration can
         * add to, and these are the program's own enums - so this pass has to see every `data` *and*
         * every written `instance`, because a program that declared `instance Eq(Colour)` gets its
         * own and not the compiler's. Skipping a head the table already holds is what states that,
         * where the shipping stage states it by consulting its hooks only after a lookup failed.
         */
        resolver.provideEnumInstances();

        drainDeclarationQueues();

        /*
         * Every instance's superclasses, now that there is nothing left to declare.
         *
         * Its own pass for the reason the whole phase order exists: an answer that depends on how
         * much of the program has been read is an answer that depends on the order it was written
         * in. `checkInstanceSuperclasses` is where that is argued.
         */
        resolver.checkInstanceSuperclasses();
    };

    /*
     * The layout cycles, and then what every type owes - `own.h`, where the order is argued.
     *
     * Here rather than beside the field walk because the cut has to see the whole member graph: a
     * record may name one declared further down, and an instantiation `declareRecordFields` made is
     * only filled by `completeInstantiations`. This is the first point at which every field of every
     * type in the program is in, which is the earliest a containment cycle can be seen at all - with
     * the one exception the drain below closes, a `data` whose field is a declaration path and whose
     * fields are therefore read after this.
     */
    auto classified = false;
    auto settleTypes = [&]() {
        if(classified) return true;
        classified = true;

        if(!breakLayoutCycles(context, *program)) return false;
        if(!classifyTypes(context, *program)) return false;

        /*
         * What a qualified type argument may be written over - `checkQualifiedShapes`.
         *
         * Immediately after the classification and before the bodies, because that is the first
         * point the answer exists and the last point before anything is built against it: what a
         * `Maybe('v)` releases is a question about `Maybe`'s members, and whether `Maybe` has any is
         * what the pass above settles.
         */
        return checkQualifiedShapes(context, *program);
    };

    auto initialized = false;
    auto settleGlobals = [&]() {
        if(initialized) return;
        initialized = true;

        resolver.eachFile([&](ast::Module& file) { resolver.resolveInitializers(file); });

        // The half of `pub` that had to wait for them - a global's type is what its initializer said.
        checkExportedGlobals(context, *program);
    };

    /*
     * A program that wrote a declaration path: everything the drain's demanded bodies read, first.
     *
     * The three groups in their own order, which is the order they are written in below. The cost is
     * that a program with a path sees the checkpoint further down than one without - so a broken one
     * collects the classification's diagnostics alongside the declarations' - and the benefit is
     * that a producer named in a signature is resolved against the same program every other body is.
     */
    if(resolver.hasDeferredSitePaths()) {
        settleDeclarations();
        if(!settleTypes()) return program;

        /*
         * The initializers, **unless the declarations are already broken** - which is the checkpoint
         * further down, asked here for the one group that runs above it.
         *
         * A global's initializer is code, and code resolved against a declaration that failed reports
         * about the failure rather than about itself: `data Bad {field: NoSuchType}` leaves `Bad` with
         * no field of that name, so `let &g = Bad {field: 1}` beside it was "Bad has no field called
         * field" - of a declaration that plainly writes one. Without a path in the module the
         * checkpoint had already stopped the compile and only the true diagnostic was reported.
         *
         * Skipped rather than reordered, because the pass below needs what it needs: a demanded body
         * reads a global's type, so this group cannot move after `resolveSiteSignatures`. A program
         * that is not going to compile does not need it to have run - the checkpoint returns before
         * any body - and the latch means a program that *is* runs it in its usual place.
         *
         * `context.index` keeps an editor's compile on the old path for the reason the checkpoint
         * itself makes an exception for one: a file being written is a file with a mistake in it, and
         * what it asks is what the rest of the file means.
         */
        if(!context.diagnostics.errorCount() || context.index) settleGlobals();
    }

    /*
     * The headers that name a **declaration path** - `f.Iter`, `f.Result`, §2.4 of
     * Design-Iterator-Objects.md - and therefore had to wait for all of them.
     *
     * Here and nowhere else: what such a header names is a type built out of somebody else's body,
     * so it needs every function in the program to be a name with a signature behind it (which is
     * what the pass above finishes) and it has to be in before anything reads a signature (which is
     * everything below). `resolveAliasTargets` is the same pass one phase up, for the same reason.
     */
    resolver.resolveSiteSignatures();

    settleDeclarations();

    /*
     * And the queues that pass just filled - the fields of every `data` it unblocked, and therefore
     * the defaults on them. The call above is a no-op for a program that reached the group early,
     * which is exactly the program this line exists for.
     */
    drainDeclarationQueues();

    /*
     * `pub`, from the declaring side - `export.cpp`.
     *
     * Here because an exported *interface* is finished here: every signature, every field and every
     * global's type is in, and a body has nothing to say about who may name what. Before the
     * checkpoint below rather than after it, so that a leak is reported alongside the declaration
     * diagnostics it belongs with instead of being reached only by a program that had none.
     */
    checkExports(context, *program, toBuffer(groups));

    /*
     * The one checkpoint an editor's compile does not stop at.
     *
     * A file being written is a file with a mistake in it, and what an editor asks is what the rest of
     * it means - so with an index attached the bodies are resolved against whatever declarations did
     * resolve, and a body that reports because of a broken one is dropped before the analyses rather
     * than handed to them (see `Resolver::resolveBody`). Every other checkpoint below is a pass
     * refusing its own input and stops an editor's compile exactly as it stops a build.
     */
    if(context.diagnostics.errorCount() && !context.index) return program;

    if(!settleTypes()) return program;
    settleGlobals();

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
    // Under `-test` the entry is the suite rather than `main` - Design-Test.md §3.3's third rule, and
    // resolve_test.cpp. Both run the root's top level first, which is what makes the choice a tail.
    if(context.settings.test) resolver.resolveTestEntry();
    else resolver.resolveProgramEntry();

    resolver.eachFile([&](ast::Module& file) { resolver.resolveBodies(file); });

    // A completion request stops here - Implementation-Tooling.md §8.2. What it answers was recorded
    // while the body holding the cursor was resolved, and the analyses answer nothing it asks.
    if(context.completion) return program;

    // The string globals, which are the one constant whose value is a *body's* answer - see
    // `PendingTextGlobal`. Here because this is the first point at which the library's own string
    // builder has one, and before anything reads what a global holds.
    resolver.resolveTextGlobals();

    // And the string *tables* - a `map` whose arms are literals, on exactly the same terms and for
    // exactly the same reason. See `PendingTextMap`.
    resolver.resolveTextMaps();

    // The one question a class declaration cannot be asked before its defaults have bodies - see
    // `checkClassDefaults`. Here rather than inside the walk above, because a default may call a
    // member declared below it and a per-declaration check would answer against half a class.
    resolver.checkClassDefaults();

    /*
     * And the **boxed** recursive nests, now that the last object in the program has been minted -
     * `completeBoxedNests`, Design-Iterator-Objects.md §6.3.
     *
     * Here and nowhere earlier: a boxed member names the child's *producer* at the mint and the
     * child's *record* only once every mint has finished, which for a mutually recursive pair is
     * after the outer one returns. Before `freezeSchemas` below, because the pass declares the step
     * shim each box is called across and a function declared after the numbering is one nothing
     * counts.
     */
    resolver.completeBoxedNests();

    /*
     * The canonical slot numbering, derived once every context is finished - `gen.h`.
     *
     * After the bodies and not after the signatures, because a body is the other source of
     * requirements: what a generic body dispatches on is part of its environment, and a numbering
     * frozen before the body had been read would be one the body then invalidated. Nothing this
     * milestone builds adds a requirement from a body, and the call is placed where it will still be
     * right when something does - which is the point of freezing at all rather than caching.
     */
    ErrorMark frozen(context);
    freezeSchemas(context, *program);

    // An editor's compile has errors by definition, so it stops only on what this pass reported.
    if(context.index ? frozen.reported() : context.diagnostics.errorCount() != 0) return program;

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
    ErrorMark forms(context);
    selectGenForms(context, *program);
    if(context.index ? forms.reported() : context.diagnostics.errorCount() != 0) return program;

    computeSummaries(context, *program);

    /*
     * Whether a boxed field needs a provenance bit at all - `settleBoxProvenance`.
     *
     * After the summaries because it is a question about the *reachable* bodies and reachability is
     * what that walk decides, and before anything asks for a layout because the answer is half of
     * `ssa::storesBoxProvenance` and a `Repr` is memoized on the record it describes.
     */
    settleBoxProvenance(*program);

    // Every specialization the walks above made, against the generic it came from - `verify.h`. Last
    // because it is the only check whose subject does not exist until the walks have run.
    checkSpecializations(context, *program);

    /*
     * And the resolver stops answering, which is the honest statement about its own lifetime: it is
     * a local of this function and the conversion runs after it, so a hook left set would be a
     * pointer into a dead frame. What still needs an object of a class member below this line finds
     * it on `Function::iterMaker`, where the walks above put it.
     *
     * A new protocol below this line has nowhere for its `Iterate` instance to come from for the
     * same reason - `mir::erasedIterTypeOf` refuses one by name.
     */
    program->iterObjects = nullptr;
    program->erasedItersClosed = true;
    return program;
}

Ptr<Program> resolveProgram(Context& context, ast::Module& root, ast::ModuleProvider* provider) {
    ast::ModuleGroup group { root.name };
    group.files.push(&root);
    return resolveProgram(context, group, provider);
}

} // namespace mir
