/*
 * Written types: turning what the source says into a type.
 *
 * This is the only file that sees an `ast::Type`, which is the point of separating it: everything
 * below the seam takes types it is handed and cannot be reached from a syntax it has to interpret.
 * The attributes are here too - `@bits`, `@box`, `@inline`, `@capacity`, a fixed array's length -
 * because each of them exists only in written form, and what they produce is an ordinary type the
 * rest of the system cannot tell was refined.
 */

#include "type_internal.h"
#include "generic.h"
#include "module.h"
#include "name.h"
#include "index.h"

static bool readBoxAttribute(Module& module, const ast::Type& type);

static bool hasAttribute(Module& module, ast::ParsePtr<ast::AttrList> attributes, const char* name, U32 length);

static TypePtr resolveTupleAst(Module& module, const ast::Type& type, GenEnv* env) {
    auto parseBase = module.parse;
    SmallArray<Field, 8> fields;
    auto astFields = type.tup.fields;

    for(auto astField: astFields.contents(parseBase)) {
        auto boxed = readBoxAttribute(module, astField.type);
        auto declared = astField.type;

        /*
         * The attribute is spent here, so the type is resolved without it.
         *
         * That is the whole of why `@box` is not a type refinement the way `@bits` is: the field's
         * declared type is what the field holds, and everything downstream - `f(cfg.cold)`, a
         * pattern binding, a diagnostic - is entitled to see exactly what was written. Stripping
         * the list rather than only the one attribute is safe because readBoxAttribute has already
         * rejected the one combination that could have been in it.
         */
        if(hasAttribute(module, declared.attributes, "box", 3)) declared.attributes = nullptr;

        fields.push(Field { resolveType(module, declared, env), astField.name, boxed });
    }

    return (Type*)resolveTupleType(module, toBuffer(fields), type.source) - *module.types;
}

/*
 * A written function type - `(&a: Int, return b: T) -> &T`.
 *
 * The conventions and the `return` markers are read here rather than dropped, which is the whole
 * point of Implementation-IR.md part 3's "the natural home is FunArg": a caller holding one of these
 * has to know what the callee does to each argument, and this is the only place it can find out.
 * The same validity rules apply as to a declaration, so both go through checkReturnRoot.
 */
/*
 * What a parameter's written type means, which is not always what the same syntax means elsewhere.
 *
 * `[T]` in a *binding* position is a slice - Implementation-Containers.md §4. The default binding
 * convention is an immutable borrow and `&` makes it a mutable one, and what a borrow of a
 * contiguous container *is* is a `{base, length}` descriptor rather than an address of the owner.
 * That is the one fixed and universal thing in the container design: a borrow of `[T]` has one
 * concrete representation and never dispatches, which is what makes "no polymorphic calls by
 * default" true by construction.
 *
 * Three positions deliberately keep the owner:
 *
 *  - `->xs: [T]`, which consumes the container rather than looking at it;
 *  - a field, a `::` ascription and a return type, which are *type* positions and have no
 *    convention to read - `data F {xs: [T]}` owns an array, and `data F {xs: &[T]}` is how a stored
 *    slice is spelled (see resolveType's Borrow case);
 *  - `xs: Array(T)` written out, which is how Collections' own operations name the growable type.
 *    Growth is nominal, because only the growable type can grow: `push` says `Array(T)` and `sort`
 *    says `[T]`, and the difference between them is exactly this function.
 *
 * `[T *n]` in a binding position goes the same way, which is §6's "as an immutable argument it
 * produces a slice; as a mutable-element argument a mutable slice - both free, no coercion, no
 * specialization". The one thing it is never is a *growable* argument: `push` says `Array(T)`, so a
 * fixed array reaching it is rejected by the ordinary conversion rule with a diagnostic naming why -
 * see convertType.
 */
TypePtr bindingType(Module& module, const ast::Type& written, ast::BindType bind, GenEnv* env) {
    auto type = resolveType(module, written, env);
    if(bind == ast::BindType::Sink) return type;
    if(written.kind != ast::Type::Arr) return type;

    auto slice = sliceOf(module, type);
    return slice ? slice : type;
}

static TypePtr resolveFunTypeAst(Module& module, const ast::FunType& type, GenEnv* env, LocationId source) {
    auto parseBase = module.parse;
    SmallArray<FunArg, 8> args;
    auto allRootsMutable = true;
    auto roots = 0u;
    auto written = 0u;
    U32 index = 0;

    auto declaredArgs = type.args;

    for(auto declared: declaredArgs.contents(parseBase)) {
        FunArg arg;
        arg.type = bindingType(module, declared.type, declared.bind, env);
        arg.name = declared.name;
        arg.convention = declared.bind;
        arg.lazy = declared.lazy && checkLazyArgument(module, declared.bind, declared.returnRoot, source);

        if(declared.returnRoot) {
            written++;

            if(checkReturnRoot(module, arg.type, declared.bind, index, source)) {
                arg.returnRoot = true;
                roots++;
                if(declared.bind != ast::BindType::Ref) allRootsMutable = false;
            }
        }

        args.push(arg);
        index++;
    }

    auto result = resolveType(module, type.ret, env);

    if(isBorrow(*module.types, result)) {
        if(!roots && !written) {
            module.context.diagnostics.error("a function type returning a borrow must mark the argument it is rooted in with `return`"_v,
                                             source);
        } else if(roots) {
            result = applyReturnRootMutability(module, result, allRootsMutable);
        }
    }

    return resolveFunType(module, toBuffer(args), result, type.kind);
}

static TypePtr resolveAlias(Module& module, TypeAlias& alias, Buffer<TypePtr> args, LocationId source);

// A named type with no arguments. A generic declaration written bare is an error rather than a
// partial application: higher-kinded use is a Milestone 2 concern and silently accepting it here
// would produce a type with no Repr much later.
static TypePtr resolveNamed(Module& module, StringId name, LocationId source) {
    auto global = *module.types;

    if(auto alias = findAlias(module, name, source)) {
        return resolveAlias(module, *alias.unwrap(), {}, source);
    }

    auto type = findType(module, name, source);
    if(!type) {
        module.context.diagnostics.error("unknown type %@"_v, source, module.context.findName(name));
        return module.scalar.error;
    }

    if(global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];
        if(record->gen && global[record->gen]->types.size()) {
            module.context.diagnostics.error("type %@ requires type arguments"_v, source, module.context.findName(name));
            return module.scalar.error;
        }
    }

    return type;
}

static TypePtr resolveApp(Module& module, const ast::AppType& app, GenEnv* env, LocationId source) {
    auto global = *module.types;

    if(app.base.kind != ast::Type::Con) {
        return errorType(module, source, "only a named type can be applied to type arguments"_v);
    }

    TypeList args;
    auto appArgs = app.args;
    for(auto arg: appArgs.contents(module.parse)) args.push(resolveType(module, arg, env));

    if(auto alias = findAlias(module, app.base.name, source)) {
        return resolveAlias(module, *alias.unwrap(), toBuffer(args), source);
    }

    auto type = findType(module, app.base.name, source);
    if(!type) {
        module.context.diagnostics.error("unknown type %@"_v, source, module.context.findName(app.base.name));
        return module.scalar.error;
    }

    if(global[type]->kind != Type::Record) {
        return errorType(module, source, "only a data type can take type arguments"_v);
    }

    return instantiateRecord(module, ((RecordType*)global[type])->base(global), toBuffer(args), source);
}

static TypePtr resolveAlias(Module& module, TypeAlias& alias, Buffer<TypePtr> args, LocationId source) {
    auto global = *module.types;
    auto expected = alias.gen ? global[alias.gen]->types.size() : 0;

    if(expected != args.length) {
        module.context.diagnostics.error("alias %@ takes %@ arguments but was given %@"_v, source,
                                         module.context.findName(alias.name), U32(expected), U32(args.length));
        return module.scalar.error;
    }

    if(!alias.resolved) {
        if(alias.resolving) {
            module.context.diagnostics.error("alias %@ is defined in terms of itself"_v, source,
                                             module.context.findName(alias.name));
            return module.scalar.error;
        }

        // The target is resolved in the module that declared the alias, so the names it uses
        // mean what they meant there rather than what they happen to mean here.
        auto& owner = *alias.module;
        alias.resolving = true;
        alias.resolved = resolveType(owner, owner.parse[alias.ast]->alias.target,
                                     alias.gen ? global[alias.gen] : nullptr);
        alias.resolving = false;
    }

    return expected ? substituteType(module, alias.resolved, args, source) : alias.resolved;
}

/*
 * The `@bits(n)` an attribute list carries, or zero.
 *
 * Everything else written as an attribute on a type is left alone rather than rejected, because the
 * grammar accepts `@name(args)` in this position for features that do not exist yet and turning
 * "not implemented" into "not allowed" here would have to be undone by each of them. `@box` is the
 * one that has landed, and it is read by readBoxAttribute below rather than here, because it
 * refines the *field* and not the type.
 */
static bool readCountAttribute(Module& module, ast::ParsePtr<ast::AttrList> attributes, LocationId source,
                               const char* name, U32 nameLength, U32& count) {
    if(!attributes) return false;

    auto parse = module.parse;
    auto wanted = module.context.addUnqualifiedName(name, nameLength);

    for(auto attribute: module.parse[attributes]->contents(parse)) {
        if(attribute.name != wanted) continue;

        auto args = attribute.args;
        if(args.size() != 1) {
            module.context.diagnostics.error("@%@ takes one argument: a literal count"_v, source,
                                             StringView { name, nameLength });
            return false;
        }

        // The literal's own kind is encoded in the expression kind - see ast::Expr::Lit - so an
        // integer literal is exactly `Lit + Literal::Int`.
        auto argument = args.get(parse, 0).value;
        if(argument.kind != ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
            module.context.diagnostics.error("@%@ takes a literal count"_v, attribute.source,
                                             StringView { name, nameLength });
            return false;
        }

        auto written = argument.lit.i();
        if(written < 0) {
            module.context.diagnostics.error("@%@ cannot be negative"_v, attribute.source,
                                             StringView { name, nameLength });
            return false;
        }

        // Reported separately from "there is no attribute" so that `@bits(0)` reaches the range
        // check rather than being read as an absent refinement.
        count = U32(written);
        return true;
    }

    return false;
}

static bool readBitsAttribute(Module& module, ast::ParsePtr<ast::AttrList> attributes, LocationId source,
                              U32& bits) {
    return readCountAttribute(module, attributes, source, "bits", 4, bits);
}

/*
 * `@box` on a field, which is a statement about the field's storage rather than about its type.
 *
 * It is read here, next to `@bits`, because the two are written in the same position and are the
 * same shape of thing - a declaration-site annotation that changes a field's physical
 * representation and that generic code sees straight through. What they differ in is the axis:
 * `@bits` narrows the width and produces a distinct type, `@box` moves the storage out of line and
 * produces a distinct *field*. So this one never reaches `resolveType`, and `cfg.cold` keeps
 * whatever type was written after the attribute.
 *
 * Rejecting the pair is not tidiness. A `@bits` field lives inside a word shared with its
 * neighbours and has no address of its own; a boxed one *is* an address. There is no representation
 * that is both, so a program asking for both is asking for something that does not exist.
 */
static bool hasAttribute(Module& module, ast::ParsePtr<ast::AttrList> attributes, const char* name,
                         U32 length) {
    if(!attributes) return false;

    auto parse = module.parse;
    auto wanted = module.context.addUnqualifiedName(name, length);

    for(auto attribute: parse[attributes]->contents(parse)) {
        if(attribute.name == wanted) return true;
    }

    return false;
}

static bool readBoxAttribute(Module& module, const ast::Type& type) {
    auto attributes = type.attributes;
    if(!attributes) return false;

    auto parse = module.parse;
    auto box = module.context.addUnqualifiedName("box", 3);
    auto bits = module.context.addUnqualifiedName("bits", 4);
    auto boxed = false;
    auto narrowed = false;

    for(auto attribute: parse[attributes]->contents(parse)) {
        if(attribute.name == bits) narrowed = true;
        if(attribute.name != box) continue;

        if(attribute.args.size()) {
            module.context.diagnostics.error("`@box` takes no arguments"_v, attribute.source);
            continue;
        }

        boxed = true;
    }

    if(boxed && narrowed) {
        module.context.diagnostics.error("`@box` and `@bits` cannot both apply to one field - a narrowed field shares a word with its neighbours and has no address of its own, and a boxed field is one"_v,
                                         type.source);
        return false;
    }

    return boxed;
}

/*
 * The `n` of a written `[T *n]`, or nothing where it is not a number this stage can read.
 *
 * A literal and nothing else, which is Implementation-Containers.md §6's "typing is resolve-stage":
 * because `n` is a literal in every position that mentions it, nothing needs to quantify over it -
 * the resolver checks a literal's length against an expected `[T *n]` and `[T *n]` never appears in
 * an instance head. A length that is a `let`, an expression or a type variable is const generics,
 * which is a separate feature and is named in the diagnostic rather than silently accepted as
 * whatever the parser happened to fold.
 */
static Maybe<U32> fixedArrayLength(Module& module, const ast::Expr& length) {
    // The literal's own kind is encoded in the expression kind - see ast::Expr::Lit - so an integer
    // literal is exactly `Lit + Literal::Int`, the same shape readBitsAttribute reads.
    if(length.kind != ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
        module.context.diagnostics.error("a fixed array's length must be an integer literal - a length that is computed or generic needs const generics, which does not exist yet"_v,
                                         length.source);
        return Nothing();
    }

    auto written = length.lit.i();
    if(written < 0) {
        module.context.diagnostics.error("a fixed array's length cannot be negative"_v, length.source);
        return Nothing();
    }

    if(written > kMaxFixedArrayLength) {
        module.context.diagnostics.error("a fixed array may hold at most %@ elements, and this one asks for %@"_v,
                                         length.source, U32(kMaxFixedArrayLength), written);
        return Nothing();
    }

    return Just(U32(written));
}

/*
 * Which row of Implementation-Containers.md §7.1 was written, and whether it is one that exists.
 *
 * Four rows, one built. `@inline(i) @capacity(i)` is the row that never spills: the inline storage is
 * exactly `i * stride`, there is no capacity field because the capacity *is* `i`, and there is no
 * spill pointer because there is no spill - which is why the build order names it first. The other
 * three all share one missing piece, the discriminant that says whether the bytes currently hold
 * elements or a pointer to them, so they are refused by name rather than laid out wrong.
 *
 * The refusal is a diagnostic and not a silent fallback to the plain array, because these are ABI
 * annotations: a field whose layout quietly stopped being what was written is the one failure a Repr
 * refinement must not have.
 */
static TypePtr resolveContainerRefinement(Module& module, TypePtr plain, bool hasInline, U32 inlineSlots,
                                          bool hasCapacity, U32 capacityBound, LocationId source) {
    if(!plain || (*module.types)[plain]->kind == Type::Error) return plain;

    if(!arrayElement(module, plain)) {
        module.context.diagnostics.error("`@inline` and `@capacity` refine a growable array - `[T]` - and this is %@"_v,
                                         source, describeType(module.context, *module.types, plain));
        return plain;
    }

    if(!hasInline || !hasCapacity || inlineSlots != capacityBound) {
        module.context.diagnostics.error("only `@inline(n) @capacity(n)` is built - the rows that spill need a discriminant saying whether the inline bytes hold elements or a pointer to them, and that is Implementation-Containers.md §7.1's unbuilt half"_v,
                                         source);
        return plain;
    }

    if(!inlineSlots) {
        module.context.diagnostics.error("`@inline(0) @capacity(0)` is an array that can hold nothing - write `[T]` for one that allocates, or a larger bound"_v,
                                         source);
        return plain;
    }

    if(inlineSlots > kMaxInlineSlots) {
        module.context.diagnostics.error("`@inline` may hold at most %@ elements, and this one asks for %@ - past that the storage belongs in an allocation rather than inside its owner"_v,
                                         source, U32(kMaxInlineSlots), inlineSlots);
        return plain;
    }

    return refineContainerType(module, plain, inlineSlots, capacityBound, source);
}

TypePtr resolveType(Module& module, const ast::Type& type, GenEnv* env) {
    /*
     * Anything reaching here still carrying `@box` is one written somewhere a field is not, since
     * resolveTupleAst strips the attribute off the fields it consumed.
     *
     * Reported rather than ignored because the two readings of `let x: @box T` are far apart and
     * neither is this: it is either a boxed *local*, which is a real feature nothing implements yet,
     * or a boxed type, which is precisely what an edge annotation exists not to be. Silently
     * dropping it would compile a program to something other than what it says.
     */
    if(hasAttribute(module, type.attributes, "box", 3)) {
        module.context.diagnostics.error("`@box` can only be written on a field of a record or tuple - it is a statement about where a field's storage lives, not a type"_v,
                                         type.source);

        auto plain = type;
        plain.attributes = nullptr;
        return resolveType(module, plain, env);
    }

    U32 bits = 0;
    if(readBitsAttribute(module, type.attributes, type.source, bits)) {
        // Resolved without the attribute first, so that `@bits(4) UInt` narrows whatever `UInt`
        // turned out to be rather than needing a case per way of spelling an integer.
        auto plain = type;
        plain.attributes = nullptr;
        return resolveBitsType(module, resolveType(module, plain, env), bits, type.source);
    }

    /*
     * `@inline(i) @capacity(c) [T]` - Implementation-Containers.md §7.
     *
     * Read together rather than one at a time, because the *pair* is what selects a layout: §7.1 is
     * a table of four rows and which row this is depends on both numbers. The two are written as
     * separate attributes because they are separate statements - one is about storage and one is
     * about a bound - and either alone is a legal row of that table.
     *
     * The underlying type is resolved without them first, exactly as `@bits` does, so that the
     * refinement applies to whatever `[T]` turned out to be rather than needing an arm per spelling.
     */
    U32 inlineSlots = 0;
    U32 capacityBound = 0;
    auto hasInline = readCountAttribute(module, type.attributes, type.source, "inline", 6, inlineSlots);
    auto hasCapacity = readCountAttribute(module, type.attributes, type.source, "capacity", 8, capacityBound);

    if(hasInline || hasCapacity) {
        auto plain = type;
        plain.attributes = nullptr;
        return resolveContainerRefinement(module, resolveType(module, plain, env), hasInline, inlineSlots,
                                          hasCapacity, capacityBound, type.source);
    }

    switch(type.kind) {
        case ast::Type::Error:
            return module.scalar.error;
        case ast::Type::Unit:
            return module.scalar.unit;
        case ast::Type::Con:
            return resolveNamed(module, type.name, type.source);
        case ast::Type::Gen: {
            auto found = env ? genVariable(module, *env, type.name, type.source) : nullptr;
            if(!found) {
                module.context.diagnostics.error("unknown type variable %@ - it is not declared in this context"_v,
                                                 type.source, module.context.findName(type.name));
                return module.scalar.error;
            }

            // `a` in a signature jumps to its binder, which is wherever the context first named
            // it - §1.2's type variable choke point.
            recordReference(module.context, type.source, typeVarSymbol(module, found),
                            (Type*)(*module.types)[found] - *module.types);

            return (Type*)(*module.types)[found] - *module.types;
        }
        case ast::Type::App:
            return resolveApp(module, *module.parse[type.app], env, type.source);
        case ast::Type::Tup:
            return resolveTupleAst(module, type, env);
        case ast::Type::Ptr:
            return resolvePointerType(module, resolveType(module, *module.parse[type.to], env));
        case ast::Type::Arr: {
            // `[T]` is the growable array, which is an ordinary generic record declared in
            // Collections rather than a type kind: the grammar has a spelling for it, and what the
            // spelling means is a library type. `[T *n]` is the other way round - it is a kind of
            // its own, because what it differs in is capability rather than layout (§6).
            if(type.arr.length) {
                auto element = resolveType(module, *module.parse[type.arr.type], env);
                auto length = fixedArrayLength(module, *module.parse[type.arr.length]);
                if(!length) return module.scalar.error;

                return resolveFixedArrayType(module, element, length.unwrap(), type.source);
            }

            if(!module.program.arrayType) {
                return errorType(module, type.source, "arrays are not available in this module"_v);
            }

            auto element = resolveType(module, *module.parse[type.arr.type], env);
            return instantiateRecord(module, module.program.arrayType, { &element, 1 }, type.source);
        }
        case ast::Type::Borrow: {
            auto to = resolveType(module, *module.parse[type.to], env);

            /*
             * `&[T]` is a slice - Implementation-Containers.md §4.2.
             *
             * A field has only a type, so this is the spelling a *stored* borrow of a container has,
             * and what is stored is the descriptor rather than an address of the owner. It is the
             * shape zero-copy parsing is written in - Design-Memory §5.3's
             * `data Parser {input: &String, pos: Int}` with an array in it - and it is tracked by
             * ordinary last-use liveness with no lifetime parameter on the record.
             *
             * There is deliberately no spelling for a stored *mutable* slice: a field has no
             * return-root group to confer exclusivity, which is the existing borrow model's rule
             * rather than anything about arrays.
             */
            if(auto slice = sliceOf(module, to)) return slice;

            // Immutable until the signature it belongs to says otherwise: what makes a returned
            // borrow exclusive is the return-root group being entirely `return &`, which is not
            // known until every argument of the declaration has been read.
            return resolveBorrowType(module, to, false);
        }
        case ast::Type::Fun:
            return resolveFunTypeAst(module, *module.parse[type.fun], env, type.source);
        default:
            return errorType(module, type.source, "type is not available in this milestone"_v);
    }
}

/*
 * What may carry the `@lazy` marker.
 *
 * Both rules follow from the argument not being evaluated. A `&` or `->` parameter is a statement
 * about storage the caller already has - one to write through, one to hand over - and there is no
 * such storage until the expression runs, which the callee may decline to do. `return` says a
 * borrow in the result may be rooted in the argument, and an argument that may never exist cannot
 * root anything.
 */
bool checkLazyArgument(Module& module, ast::BindType convention, bool returnRoot, LocationId source) {
    if(convention != ast::BindType::Borrow) {
        module.context.diagnostics.error("`@lazy` cannot be combined with `&` or `->` - the argument is an expression the callee may never run, so there is no caller storage to borrow or to consume"_v,
                                         source);
        return false;
    }

    if(returnRoot) {
        module.context.diagnostics.error("`@lazy` cannot be combined with `return` - an argument that may never be evaluated cannot be what a borrow in the result is rooted in"_v,
                                         source);
        return false;
    }

    return true;
}

/*
 * What may carry the `return` marker, per Design-Memory §5.2.
 *
 * All three rules are about the same thing: the marker says a borrow in the result may be rooted in
 * the caller's storage for this argument, so the argument has to *have* caller storage that
 * survives the call. A sunk one does not - the callee owns it - and a TrivialCopy one passed by the
 * default convention does not either, since what the body sees is a copy of its own.
 */
bool checkReturnRoot(Module& module, TypePtr type, ast::BindType convention, U32 index, LocationId source) {
    auto base = *module.types;

    if(convention == ast::BindType::Sink) {
        module.context.diagnostics.error("`return` cannot be written on a `->` argument - the callee owns what it was given, so there is no caller-side storage left for a result to be rooted in"_v,
                                         source);
        return false;
    }

    // The one rule directness decides - see arrivesAsCopy, which carries the whole of why. `return %a`
    // is how Native's `borrow` says that its result points into whatever it was given, which is the
    // one bridge from unchecked memory back into checked borrows, and is why a raw pointer is exempt.
    if(convention != ast::BindType::Ref && arrivesAsCopy(base, type)) {
        module.context.diagnostics.error("`return` on %@ has nothing to root a borrow in - it arrives in a register, so the body sees a copy of its own; write `return &` when the caller's storage must be the root"_v,
                                         source, describeType(module.context, base, type));
        return false;
    }

    // The group is a bit set, and a signature this wide has never been written. Saying so is better
    // than silently dropping a marker the caller would then rely on.
    if(index >= 64) {
        module.context.diagnostics.error("`return` cannot be written past the 64th argument"_v, source);
        return false;
    }

    return true;
}

TypePtr applyReturnRootMutability(Module& module, TypePtr result, bool allRootsMutable) {
    if(!allRootsMutable || !isBorrow(*module.types, result)) return result;
    return resolveBorrowType(module, ((BorrowType*)(*module.types)[result])->to, true);
}
