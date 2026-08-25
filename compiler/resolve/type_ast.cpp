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
static bool readHostAttribute(Module& module, const ast::Type& type);

static bool hasAttribute(Module& module, ast::ParsePtr<ast::AttrList> attributes, const char* name, U32 length);

// Defined beside the fixed array's count, which is the other position that reads one - §2.5.
static TypePtr resolveCountVariable(Module& module, GenEnv* env, StringId name, LocationId source);
static TypePtr resolveCountLiteral(Module& module, I64 written, U32 limit, LocationId source,
                                   StringView what);

/*
 * Where the `@host` fields of a tuple may sit, which is after the one field that is stored.
 *
 * The elision makes the tuple *be* its remaining field, exactly as a one-field tuple already is on a
 * target with no layout - so there has to be exactly one such field, and the properties have to hang
 * off it rather than the other way round. Pinning it to field zero is not a restriction anything
 * pays for: `{items: %a, @host length: Count}` is the declaration, and it is also the only order a
 * reader would write. What it buys is that every place that already answers "a transparent tuple is
 * its first field" keeps answering it - a constant's first component, a build plan's, a place walk's
 * - instead of each learning to look up which field the stored one is.
 */
static void checkHostFields(Module& module, Buffer<Field> fields, LocationId source) {
    auto elided = false;
    for(auto field: fields) elided = elided || field.host;
    if(!elided) return;

    if(fields.length < 2 || fields[0].host) {
        module.context.diagnostics.error("`@host` names a property of the value the record holds, so it has to follow the one field that holds it - field zero of the record, which may not itself be `@host`"_v,
                                         source);
        return;
    }

    for(Size i = 1; i < fields.length; i++) {
        if(fields[i].host) continue;

        module.context.diagnostics.error("a record with a `@host` field has exactly one stored field, and this one has more - every field after the first has to be a host property of it"_v,
                                         source);
        return;
    }

    // Named, because the name *is* the property: `length` is elided onto `arr.length`, and a
    // positional field has nothing for the host to be asked for.
    for(Size i = 1; i < fields.length; i++) {
        if(fields[i].name) continue;

        module.context.diagnostics.error("a `@host` field is reached by its own name and so has to have one"_v,
                                         source);
        return;
    }
}

static TypePtr resolveTupleAst(Module& module, const ast::Type& type, GenEnv* env) {
    auto parseBase = module.parse;
    SmallArray<Field, 8> fields;
    auto astFields = type.tup.fields;

    for(auto astField: astFields.contents(parseBase)) {
        auto boxed = readBoxAttribute(module, astField.type);
        auto host = readHostAttribute(module, astField.type);
        auto declared = astField.type;

        /*
         * The attribute is spent here, so the type is resolved without it.
         *
         * That is the whole of why `@box` is not a type refinement the way `@bits` is: the field's
         * declared type is what the field holds, and everything downstream - `f(cfg.cold)`, a
         * pattern binding, a diagnostic - is entitled to see exactly what was written. Stripping
         * the list rather than only the one attribute is safe because readBoxAttribute has already
         * rejected the one combination that could have been in it.
         *
         * `@host` is the same shape of thing, is spent the same way, and `readHostAttribute` has
         * rejected the combinations that could have been in the list beside it.
         */
        if(hasAttribute(module, declared.attributes, "box", 3)) declared.attributes = nullptr;
        if(host) declared.attributes = nullptr;

        fields.push(Field { resolveType(module, declared, env), astField.name, boxed, host });
    }

    checkHostFields(module, toBuffer(fields), type.source);
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
 *
 * ## The one exception: a count the signature quantified over
 *
 * `fn (n: Int) firstOf(xs: [Int *n])` keeps the owner - Implementation-Const-Generics.md §1.7.
 *
 * The slice conversion is *exactly* "forget the count": `Flat(T)` carries a length field and no
 * type-level number, which is what makes it one type for every `n`. A signature that names `n` is
 * saying the opposite, and slicing it would leave the variable with nothing to bind from - so
 * `firstOf([1, 2, 3, 4])` would report "cannot infer `n`" for a call whose argument states it.
 *
 * A *written* count is unaffected, because there is nothing to infer from it: `fn f(xs: [Int *4])`
 * still takes a slice, and its four is a fact about what the caller may pass rather than something
 * the body reads back. So this is not a second rule for fixed arrays - it is the same rule, applied
 * to the one case where the conversion would destroy information the declaration asked for.
 */
TypePtr bindingType(Module& module, const ast::Type& written, ast::BindType bind, GenEnv* env) {
    auto type = resolveType(module, written, env);
    if(bind == ast::BindType::Sink) return type;
    if(written.kind != ast::Type::Arr) return type;

    auto base = *module.types;
    if(base[type]->kind == Type::Array && isGeneric(base, ((ArrayType*)base[type])->count)) return type;

    auto slice = sliceOf(module, type);
    return slice ? slice : type;
}

static TypePtr resolveFunTypeAst(Module& module, const ast::FunType& type, GenEnv* env, LocationId source) {
    auto parseBase = module.parse;
    SmallArray<FunArg, 8> args;
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
            }
        }

        args.push(arg);
        index++;
    }

    auto result = resolveType(module, type.ret, env);

    if(isBorrow(*module.types, result) && !roots && !written) {
        module.context.diagnostics.error("a function type returning a borrow must mark the argument it is rooted in with `return`"_v,
                                         source);
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
        // A head whose parameters all have defaults is applicable with no arguments at all, which is
        // what makes `Vec` a bad example and `alias Buffer(a = U8, n: Int = 64)` a good one. The
        // arity check inside reports where they do not.
        TypeList args;
        applyGenDefaults(module, alias.unwrap()->gen, args);
        return resolveAlias(module, *alias.unwrap(), toBuffer(args), source);
    }

    auto type = findType(module, name, source);
    if(!type) {
        // The two constructors with no declaration behind them, and only where nothing is declared
        // under the name - see resolveApp. Written bare they are the same mistake a bare generic
        // record is, and the message is the one that names it.
        if(name == module.program.vecTypeName || name == module.program.maskTypeName) {
            module.context.diagnostics.error("type %@ requires type arguments"_v, source,
                                             module.context.findName(name));
            return module.scalar.error;
        }

        module.context.diagnostics.error("unknown type %@"_v, source, module.context.findName(name));
        return module.scalar.error;
    }

    if(global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];

        if(record->gen && global[record->gen]->types.size()) {
            // As above: a bare name is an application of no arguments, and a declaration every one
            // of whose parameters has a default is one such application answers.
            TypeList args;
            if(!applyGenDefaults(module, record->gen, args)) {
                module.context.diagnostics.error("type %@ requires type arguments"_v, source, module.context.findName(name));
                return module.scalar.error;
            }

            return instantiateRecord(module, record->base(global), toBuffer(args), source);
        }
    }

    return type;
}

/*
 * `Vec(a)`, `Vec(a, n)` and `Mask(a)` - Implementation-Vector.md §1.4.
 *
 * Recognized by the Core name rather than by grammar, which is what keeps the parser out of this
 * entirely: a vector is a type application like any other, and the only thing unusual about it is
 * that there is no declaration behind the name. There cannot be - what a `Vec(Float)` *is* is a
 * function of the target, so there is nothing for a `data` declaration to say.
 *
 * What there *is* is a parameter list, `Program::vectorGen`, and everything below this line is
 * therefore the ordinary path. The count is read by `resolveAppArg`, which knows it is a count
 * because the parameter at that index is a `GenKind::Const` - the same question it asks of a
 * `data A(width: Int)`. The omitted second argument is filled by `applyGenDefaults` from that
 * parameter's `0`, which is the natural form already spelled out. Neither the arity check nor the
 * literal-or-variable check is written here any more, and the two messages that were are gone with
 * them: an arity mistake reads as the one every other constructor gives.
 *
 * Nothing here decides whether the element is a lane or what the natural count is - both are
 * `resolveVectorType`'s, so the two spellings and the substituted form all reach one set of rules.
 */
static Maybe<TypePtr> resolveVectorApp(Module& module, const ast::AppType& app, GenEnv* env,
                                       LocationId source) {
    auto& program = module.program;
    auto name = app.base.name;
    auto isMask = name == program.maskTypeName;
    if(!isMask && name != program.vecTypeName) return Nothing();

    TypeList args;
    auto index = Size(0);

    auto appArgs = app.args;
    for(auto arg: appArgs.contents(module.parse)) {
        args.push(resolveAppArg(module, program.vectorGen, index++, arg, env));
    }

    if(!applyGenDefaults(module, program.vectorGen, args)) {
        /*
         * Both spellings take the same two arguments, and a mask takes the count for the reason a
         * vector does: it is the shape of the vector it masks, so a mask over a lane count the
         * target did not choose has to be nameable wherever such a vector is. `Native.bits` is the
         * declaration that needed it - a movemask is written over whatever width the group is.
         */
        module.context.diagnostics.error("%@ takes a lane type, and optionally a lane count - `%@(Float)` for the target\'s natural width, `%@(Float, 4)` for exactly four"_v,
                                         source, module.context.findName(name),
                                         module.context.findName(name), module.context.findName(name));
        return Just(module.scalar.error);
    }

    return Just(resolveVectorType(module, args[0], args[1], isMask, source));
}

/*
 * One argument of a written type application, at whichever kind the declaration's parameter is.
 *
 * The count forms are exactly §2.5's two: a literal, which arrives as an `ast::Type::Lit` because
 * `parseTypeApplicationArg` makes an integer one, and a bare variable, which arrives as an
 * `ast::Type::Gen` because a lowercase type argument already parses as one. Neither needed a
 * production, which is §0.2's "the spellings already parse".
 *
 * Shared with a class constraint's argument list, which is the same question about a class's
 * parameters rather than a record's - Implementation-Const-Generics.md §10.3.
 */
TypePtr resolveAppArg(Module& module, GlobalPtr<GenEnv> declared, Size index,
                      const ast::Type& arg, GenEnv* env) {
    auto global = *module.types;
    auto parameters = declared ? global[declared]->types : GlobalList<GlobalPtr<GenType>>();

    /*
     * A position the declaration does not have, where the declaration is known: nothing to check the
     * argument against, and the arity message is the whole of what is wrong. Resolving it anyway
     * would put a second diagnostic on one mistake - `Vec(I16, 4, 4)` is one argument too many, and
     * the surplus `4` is not also "a number is not a type".
     *
     * Only where a declaration was found. A constraint over an *unknown* class has a null `declared`
     * and every position is a type position there, which is resolveConstraintArgs' deliberate
     * reading and not this case.
     */
    if(declared && index >= parameters.size()) return module.scalar.error;

    if(index >= parameters.size() || global[parameters.get(global, index)]->kind != GenKind::Const) {
        return resolveType(module, arg, env);
    }

    // The bound a count position carries is the *parameter's* type rather than a limit of this
    // constructor: a `[a *n]` inside the declaration is checked when the argument reaches it, which
    // is where the message can name the array. So the only ceiling here is the type's own.
    auto limit = maxLimit<U32>;

    if(arg.kind == ast::Type::Lit) {
        auto literal = module.parse[arg.lit];

        if(!literal || literal->kind != ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
            module.context.diagnostics.error("this parameter is a const parameter, so it takes an integer literal or a const parameter"_v,
                                             arg.source);
            return module.scalar.error;
        }

        auto count = resolveCountLiteral(module, literal->lit.i(), limit, arg.source, "a count"_v);
        return count ? count : module.scalar.error;
    }

    if(arg.kind == ast::Type::Gen) {
        auto count = resolveCountVariable(module, env, arg.name, arg.source);
        return count ? count : module.scalar.error;
    }

    /*
     * Everything else, named rather than described in the abstract - which is what resolving it is
     * for. A type that does not resolve has already said so, and following that with this would put
     * a second diagnostic on one mistake: `Vec(I16, Holder)` over a generic `Holder` is a missing
     * type argument, and it is not also a count written wrong.
     */
    auto written = resolveType(module, arg, env);
    if(global[written]->kind == Type::Error) return module.scalar.error;

    module.context.diagnostics.error("this parameter is a const parameter, so it takes an integer literal or a const parameter and not %@"_v,
                                     arg.source, describeType(module.context, global, written));
    return module.scalar.error;
}

/*
 * `a = Int`, `n: Int = 0` - Implementation-Const-Generics.md §1.8.
 *
 * Two arms because there are two kinds of parameter, and each reads its default by the rule its own
 * *arguments* are read by: a const parameter takes an integer literal, a type parameter takes a
 * type. Everything a written argument would be refused for, a default is refused for here.
 *
 * The one rule a default has that an argument does not: **it must be concrete.** A default that
 * mentioned another parameter of the same list - `data A(a, b = a)` - would be an argument whose
 * meaning depended on the order the list was filled in, and would have to be substituted through
 * every time the declaration was applied. Neither is worth a first version, and refusing it here
 * leaves the room to allow it later, which accepting the wrong reading would not.
 */
TypePtr resolveGenDefault(Module& module, GlobalPtr<GenType> variable, const ast::Type& written,
                          GenEnv* env, LocationId source) {
    auto global = *module.types;
    auto& context = module.context;
    auto name = context.findName(global[variable]->name);

    if(global[variable]->kind == GenKind::Const) {
        // The literal's kind is encoded in the expression kind, exactly as fixedArrayCount reads it.
        if(written.kind != ast::Type::Lit) {
            context.diagnostics.error("%@ is a const parameter, so its default is an integer literal"_v,
                                      written.source, name);
            return nullptr;
        }

        auto literal = module.parse[written.lit];

        if(!literal || literal->kind != ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
            context.diagnostics.error("%@ is a const parameter, so its default is an integer literal"_v,
                                      written.source, name);
            return nullptr;
        }

        // Bounded by the parameter's *type* and not by any one position that uses it, which is
        // resolveAppArg's rule and holds here for its reason: a `[a *n]` inside the declaration
        // checks the number when it reaches it, where the message can name the array.
        return resolveCountLiteral(module, literal->lit.i(), maxLimit<U32>, written.source,
                                   "a default count"_v);
    }

    if(written.kind == ast::Type::Lit) {
        context.diagnostics.error("%@ is a type parameter, so its default is a type and not a number - a parameter that takes a number is written `%@: Int`"_v,
                                  written.source, name, name);
        return nullptr;
    }

    auto type = resolveType(module, written, env);
    if(!type || global[type]->kind == Type::Error) return nullptr;

    if(isGeneric(global, type)) {
        context.diagnostics.error("the default for %@ must be a concrete type - a default that names another parameter would depend on the order the arguments were filled in"_v,
                                  written.source, name);
        return nullptr;
    }

    return type;
}

/*
 * The written defaults of a head, moved onto its variables. Once, on the first application that
 * needs them.
 *
 * In the module that *declared* the context, which is what `GenEnv::module` is carried for: a
 * default is a piece of the declaration, so the names in it mean what they meant where it was
 * written and not what they happen to mean at the application. `resolveAlias` makes the same move
 * for the same reason.
 */
void resolveGenDefaults(Module& from, GlobalPtr<GenEnv> declared) {
    auto context = (*from.types)[declared];
    if(!context || context->defaultsResolved || !context->module) return;

    auto& module = *context->module;
    auto global = *module.types;

    // Before the loop, not after it - see GenEnv::defaultsResolved.
    context->defaultsResolved = true;

    for(auto written: context->writtenDefaults.contents(global)) {
        if(written.index >= context->types.size()) continue;

        auto variable = context->types.get(global, written.index);
        auto ast = module.parse[written.written];
        if(!ast) continue;

        global[variable]->def = resolveGenDefault(module, variable, *ast, context, written.source);
    }

    /*
     * And the one rule about the list rather than about any one default: **a defaulted parameter
     * may be followed only by defaulted parameters.**
     *
     * An application writes its arguments in order and stops, so the positions it omitted are a
     * suffix and there is no spelling that skips one. A head whose defaults are not a suffix has
     * written a default nothing can ever take, and saying so here is better than an arity message
     * at every use site that reads as if the default were not there.
     *
     * Only where the arguments are positional. A function or an instance declares its parameters by
     * using them and fills them by inference, so a default of one says nothing about the others -
     * that flavour is a fallback at the settle and has no order at all. A class is neither: its
     * arguments are positional but its default is the settle's, and resolveClassDefault already
     * refuses one on a head with more than a single parameter.
     */
    if(context->kind != GenEnv::Record && context->kind != GenEnv::Alias) return;

    auto seen = false;
    for(auto variable: context->types.contents(global)) {
        auto type = global[variable];

        if(type->def) {
            seen = true;
        } else if(seen) {
            module.context.diagnostics.error("%@ has no default, and a parameter before it does - an application omits its last arguments, so a default that is not followed by defaults is one nothing can take"_v,
                                             type->source, module.context.findName(type->name));
            return;
        }
    }
}

bool applyGenDefaults(Module& module, GlobalPtr<GenEnv> declared, TypeList& args) {
    auto global = *module.types;
    if(!declared) return args.size() == 0;

    resolveGenDefaults(module, declared);

    auto parameters = global[declared]->types;
    if(args.size() > parameters.size()) return false;

    for(auto i = args.size(); i < parameters.size(); i++) {
        auto def = global[parameters.get(global, i)]->def;
        if(!def) return false;

        args.push(def);
    }

    return true;
}

static TypePtr resolveApp(Module& module, const ast::AppType& app, GenEnv* env, LocationId source) {
    auto global = *module.types;

    if(app.base.kind != ast::Type::Con) {
        return errorType(module, source, "only a named type can be applied to type arguments"_v);
    }

    /*
     * The name is resolved before the arguments are, which is the other way round from how this
     * used to read, and the reason is `Vec`.
     *
     * A vector's second argument is a *number* and not a type, so resolving the arguments first
     * would report it as one before anything had a chance to notice which constructor was being
     * applied. And the vector constructors have to be looked for *after* the ordinary lookup rather
     * than before it, or they would be reserved words: `data Mask {bits: Int}` is a declaration two
     * fixtures in this tree already make, and a builtin that shadowed it would be a name the
     * language had quietly taken.
     */
    auto alias = findAlias(module, app.base.name, source);
    auto type = alias ? nullptr : findType(module, app.base.name, source);

    if(!alias && !type) {
        if(auto vector = resolveVectorApp(module, app, env, source)) return vector.unwrap();

        module.context.diagnostics.error("unknown type %@"_v, source, module.context.findName(app.base.name));
        return module.scalar.error;
    }

    /*
     * Which of a declaration's parameters is a count, and therefore which arguments are numbers -
     * Implementation-Const-Generics.md §1.1.
     *
     * Read off the declaration rather than guessed from what was written, which is the same
     * direction §1.5 requires the annotation in a head for: `A(4)` and `A(Int)` are told apart by
     * what `A` declares and not by their own shape, and a reader looking at `data A(width: Int)` is
     * looking at the one place that says so.
     *
     * `Vec` is the one constructor that is not a declaration, and it answers this question for
     * itself in resolveVectorApp above.
     */
    GlobalPtr<GenEnv> declared = nullptr;
    if(alias) {
        declared = alias.unwrap()->gen;
    } else if(global[type]->kind == Type::Record) {
        declared = global[((RecordType*)global[type])->base(global)]->gen;
    }

    TypeList args;
    auto appArgs = app.args;
    auto index = Size(0);

    for(auto arg: appArgs.contents(module.parse)) {
        args.push(resolveAppArg(module, declared, index++, arg, env));
    }

    /*
     * The positions the application did not write, from the declaration's defaults.
     *
     * Here rather than in `instantiateRecord` or `resolveAlias`, because those two are also reached
     * from `substituteType` with a list that is already complete by construction. A default is a
     * thing a *written* application omits, so this is the only place that can omit one.
     *
     * A failure is left to the arity check below, which is the message that names the constructor
     * and both numbers - so a parameter with no default reads exactly as it did before this feature.
     */
    applyGenDefaults(module, declared, args);

    if(alias) return resolveAlias(module, *alias.unwrap(), toBuffer(args), source);

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
 * `@host` on a field, which says the field is not stored at all - Implementation-Containers.md §14's
 * elision, and `Field::host` for what it means.
 *
 * Read here beside `@box` because it is the third of the same family: an annotation written on a
 * field's type that changes the field's physical representation and that generic code sees straight
 * through. `@bits` narrows the width, `@box` moves the storage out of line, and this one removes the
 * storage entirely in favour of a property the host value already has.
 *
 * Both pairings are refused, and neither refusal is tidiness:
 *
 *  - `@bits` narrows a field into a word shared with its neighbours, and this field has no word. A
 *    host property is whatever the host says it is - a `uint32` for an array's `length` - so there
 *    is nothing for a written width to be a width *of*.
 *  - `@box` makes the field an address, and an elided field has no storage to take the address of.
 *
 * The one thing this cannot check is whether the claim is *true*, because that is a question about a
 * host value rather than about a type: see Field::host, and `hostPropertiesElided` for the rule that
 * decides where it holds.
 */
static bool readHostAttribute(Module& module, const ast::Type& type) {
    auto attributes = type.attributes;
    if(!attributes) return false;

    auto parse = module.parse;
    auto wanted = module.context.addUnqualifiedName("host", 4);
    auto box = module.context.addUnqualifiedName("box", 3);
    auto bits = module.context.addUnqualifiedName("bits", 4);
    auto elided = false;
    auto conflict = false;

    for(auto attribute: parse[attributes]->contents(parse)) {
        if(attribute.name == bits || attribute.name == box) conflict = true;
        if(attribute.name != wanted) continue;

        if(attribute.args.size()) {
            module.context.diagnostics.error("`@host` takes no arguments - the property is the field's own name"_v,
                                             attribute.source);
            continue;
        }

        elided = true;
    }

    if(elided && conflict) {
        module.context.diagnostics.error("`@host` cannot combine with `@bits` or `@box` - a field elided onto a host property has no storage of its own, so there is nothing for a written width to narrow and nothing for an indirection to point at"_v,
                                         type.source);
        return false;
    }

    return elided;
}

bool admissibleConstType(Module& module, TypePtr type, LocationId source) {
    auto base = *module.types;
    if(!type || base[type]->kind == Type::Error) return false;

    /*
     * The integer types, and nothing else in this version - §2.5.
     *
     * `Bool` costs nothing to admit and has no caller; `String` has a representation question in
     * front of it. Which types are admissible is a *semantic* rule and not a syntactic one, which is
     * why this is one predicate that can be loosened one type at a time rather than a production
     * that would have to be reopened.
     */
    if(base[type]->kind != Type::Int) {
        module.context.diagnostics.error("a const parameter is a number, and %@ is not an integer type - the integer types are what a count position takes"_v,
                                         source, describeType(module.context, base, type));
        return false;
    }

    return true;
}

/*
 * The `n` of a written `[T *n]` or of a `Vec(a, n)`, as a count - §2.5.
 *
 * A literal or a bare variable, and nothing else. `[a *(n+1)]` needs normalized const expressions or
 * `sameType`'s pointer equality stops holding, since `[a *(n+1)]` and `[a *(1+n)]` would be two
 * interned types for one type - the wall Rust's `generic_const_exprs` is still at, and not one worth
 * walking into for a first version. So a computed count is named in the diagnostic rather than
 * silently accepted as whatever the parser happened to fold.
 */
static TypePtr resolveCountVariable(Module& module, GenEnv* env, StringId name, LocationId source) {
    if(!env) {
        module.context.diagnostics.error("%@ is not a const parameter of this declaration - a count is an integer literal or a const parameter, and a local is neither"_v,
                                         source, module.context.findName(name));
        return nullptr;
    }

    /*
     * §1.5's kind inference, which is easy here in a way constraint inference is not: this position
     * wants a number, so a variable it introduces is a const one with no search and no ambiguity.
     *
     * Only for a variable this occurrence *creates*. One that already exists has a kind already -
     * from its annotation, or from the first position that used it - and disagreeing with it is
     * §2.2's collision rather than a re-inference.
     */
    auto existing = findGenVariable(module, *env, name);
    auto found = existing ? existing : genVariable(module, *env, name, source);

    if(!found) {
        module.context.diagnostics.error("unknown type variable %@ - it is not declared in this context"_v,
                                         source, module.context.findName(name));
        return nullptr;
    }

    auto variable = (*module.types)[found];

    if(!existing) {
        variable->kind = GenKind::Const;
        variable->constType = module.scalar.int_;
    } else if(variable->kind != GenKind::Const) {
        module.context.diagnostics.error("%@ is a type parameter of this declaration, so it cannot also be a count - a name stands for a type or for a number and not both"_v,
                                         source, module.context.findName(name));
        return nullptr;
    }

    recordReference(module.context, source, typeVarSymbol(module, found),
                    (Type*)variable - *module.types);

    return (Type*)variable - *module.types;
}

// A literal count, bounded and interned. `limit` is what the position accepts, so that the number
// the diagnostic names is the one that was written however large it was.
static TypePtr resolveCountLiteral(Module& module, I64 written, U32 limit, LocationId source,
                                   StringView what) {
    if(written < 0) {
        module.context.diagnostics.error("%@ cannot be negative"_v, source, what);
        return nullptr;
    }

    if(U64(written) > limit) {
        module.context.diagnostics.error("%@ may be at most %@, and this one asks for %@"_v, source,
                                         what, limit, U64(written));
        return nullptr;
    }

    return constType(module, U64(written), module.scalar.int_);
}

/*
 * The `n` of a written `[T *n]`, or null where it is not a count this stage can read.
 *
 * `parseArrayType` calls `parseExpr` after the `*`, so a variable arrives as an `Expr::Var` and a
 * literal as an `Expr::Lit`. Those are the two forms §2.5 admits; everything else is an expression
 * and is refused by name.
 */
static TypePtr fixedArrayCount(Module& module, const ast::Expr& length, GenEnv* env) {
    // The literal's own kind is encoded in the expression kind - see ast::Expr::Lit - so an integer
    // literal is exactly `Lit + Literal::Int`, the same shape readBitsAttribute reads.
    if(length.kind == ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
        return resolveCountLiteral(module, length.lit.i(), kMaxFixedArrayLength, length.source,
                                   "a fixed array's count"_v);
    }

    if(length.kind == ast::Expr::Var) {
        return resolveCountVariable(module, env, length.var, length.source);
    }

    module.context.diagnostics.error("a fixed array's count must be an integer literal or a const parameter - a count that is computed needs const expressions, which this version does not have"_v,
                                     length.source);
    return nullptr;
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

            // The other half of §2.2's kind rule: a const parameter written where a type belongs.
            // Reported here rather than accepted as a type nothing has, since the position asking is
            // the one that can say what it wanted.
            if((*module.types)[found]->kind == GenKind::Const) {
                module.context.diagnostics.error("%@ is a const parameter - a number, not a type"_v,
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
                auto length = fixedArrayCount(module, *module.parse[type.arr.length], env);
                if(!length) return module.scalar.error;

                return resolveFixedArrayType(module, element, length, type.source);
            }

            if(!module.program.arrayType) {
                return errorType(module, type.source, "arrays are not available in this module"_v);
            }

            auto element = resolveType(module, *module.parse[type.arr.type], env);
            return instantiateRecord(module, module.program.arrayType, { &element, 1 }, type.source);
        }
        case ast::Type::ArrInferred:
            // Reached only where nothing supplies a count. The two positions that do - the `::` of
            // an expression and of a constant - call `inferredArrayType` before they get here, so
            // arriving at this arm *is* the diagnostic's condition rather than a case it tests for.
            return errorType(module, type.source,
                             "a count written `_` is taken from the literal the type is written at, and there is no literal here - write the number, or move the type onto a `::` in front of one"_v);

        case ast::Type::Map: {
            // `[K: V]` is `Map(K, V)`, on exactly the terms `[T]` is `Array(T)` above: a spelling in
            // the grammar for a record declared in Collections - Implementation-Map.md §7.
            if(!module.program.mapType) {
                return errorType(module, type.source, "maps are not available in this module"_v);
            }

            TypePtr args[] = {
                resolveType(module, *module.parse[type.map.from], env),
                resolveType(module, *module.parse[type.map.to], env),
            };

            return instantiateRecord(module, module.program.mapType, { args, 2 }, type.source);
        }
        /*
         * The two reference capabilities, which differ in this and in nothing else -
         * Analysis-Borrows.md §3.2.
         *
         * `&T` is exclusive and writable; `'T` is shared. Both are written on the type, which is
         * the whole point of having two: the result of `Index.get` and the result of `Index.getMut`
         * used to be the same written type, and which of the two a caller actually got was decided
         * afterwards by `applyReturnRootMutability` reading the return-root group. A signature that
         * does not say what it hands back is not a signature, and a reference nested inside
         * `Maybe`, a field or a generic argument had no way to be promoted at all.
         *
         * `&` therefore means the same thing everywhere it appears - a parameter binding, a local
         * binding, a stored reference - and there is no `mut` keyword anywhere.
         */
        case ast::Type::Borrow:
        case ast::Type::Shared: {
            auto to = resolveType(module, *module.parse[type.to], env);
            auto mut = type.kind == ast::Type::Borrow;

            /*
             * `'[T]` is a slice - Implementation-Containers.md §4.2.
             *
             * A field has only a type, so this is the spelling a *stored* borrow of a container has,
             * and what is stored is the descriptor rather than an address of the owner. It is the
             * shape zero-copy parsing is written in - Design-Memory §5.3's
             * `data Parser {input: 'String, pos: Int}` with an array in it - and it is tracked by
             * ordinary last-use liveness with no lifetime parameter on the record.
             *
             * `&[T]` reaches the same type, and that is a representation gap rather than a ruling:
             * §4.5 wants a shared and an exclusive slice to differ in capability exactly as direct
             * references do, and a slice descriptor has nowhere to carry the bit yet. Until it does,
             * the exclusive spelling is accepted and silently means the shared one.
             */
            if(auto slice = sliceOf(module, to)) return slice;

            return resolveBorrowType(module, to, mut);
        }
        case ast::Type::Fun:
            return resolveFunTypeAst(module, *module.parse[type.fun], env, type.source);
        case ast::Type::Lit:
            // The grammar accepts a number where a type is written so that `Vec(Float, 4)` parses -
            // see ast::Type::Lit. Everywhere else it is this, which is a better message than the
            // parse error it replaced, and is what stops the form being const generics by accident.
            return errorType(module, type.source, "a number is not a type - a number is a type argument only where the declaration declared a const parameter there"_v);
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

TypePtr inferredArrayType(Module& module, const ast::Type& written, const ast::Expr& value,
                          GenEnv* env) {
    assertTrue(written.kind == ast::Type::ArrInferred);

    /*
     * An array literal and nothing else. A fill is one too - `[0] :: [U8 *_]` is a one-element
     * array, since the count it would spread into is the one this type is trying to read off it -
     * and that falls out rather than being a case: the count is the number of elements written.
     */
    if(value.kind != ast::Expr::Array) {
        module.context.diagnostics.error("a count written `_` is taken from the array literal the type is written at, and this is not one - write the number instead"_v,
                                         written.source);
        return module.scalar.error;
    }

    auto element = resolveType(module, *module.parse[written.arr.type], env);

    // Copied rather than named, because `SmallList::size` is not const and this is handed the
    // expression by reference - the same reason `resolveArray` takes its list by value.
    auto items = value.arr;
    auto count = items.size();

    if(count > kMaxFixedArrayLength) {
        module.context.diagnostics.error("a fixed array may hold at most %@ elements, and this literal has %@"_v,
                                         written.source, U32(kMaxFixedArrayLength), U64(count));
        return module.scalar.error;
    }

    return resolveFixedArrayType(module, element, U32(count), written.source);
}
