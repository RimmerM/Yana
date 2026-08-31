#include "resolve_internal.h"
#include "resolve.h"
#include "verify.h"
#include "summary.h"
#include "analyze.h"
#include "own.h"

namespace mir {

void Resolver::unsupported(LocationId source, StringView what) {
    context.diagnostics.error("the ownership resolver does not implement %@ yet"_v, source, what);
}

/*
 * A written type.
 *
 * The builtin widths, `Bool`, `{}`, `%T` and the records this module declares. A named type that is
 * none of those is a declaration this stage has no table for yet, and saying so by name is more
 * useful than an "unknown type" that could equally mean a typo.
 */
DeclPtr<Type> Resolver::resolveType(ast::Type& type) {
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
            if(auto alias = program.findAlias(here(), type.name)) {
                if(isGeneric(decl(), decl()[alias]->gen)) {
                    return applyAlias(alias, nullptr, 0, type.source);
                }

                return aliasTarget(alias);
            }

            if(auto record = program.findRecord(here(), type.name)) {
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

/*
 * The parameter one argument fills, or null where the head has no such position.
 *
 * Null rather than a report, because the arity of an application is checked once by whoever applies
 * the head - a second count here would be the same mistake reported twice, in the wrong order.
 */
static DeclPtr<Type> headParameter(DeclBase base, DeclPtr<GenContext> head, Size position) {
    if(!head || position >= base[head]->variables.size()) return nullptr;
    return base[head]->variables.get(base, position);
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
    auto parameter = headParameter(decl(), head, position);
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

    // Bounded by the parameter's own width, which is the whole of what the annotation buys: `4` at
    // an `I8` parameter and `4` at an `I32` one are two arguments, so a value that does not fit is
    // a mistake about which parameter this is rather than about the number.
    auto value = literal->lit.i();
    auto& width = *(IntType*)decl()[annotation];
    auto limit = width.bits == 64 ? ~U64(0) : ((U64(1) << (width.bits - (width.isSigned ? 1 : 0))) - 1);

    if(value > limit) {
        context.diagnostics.error("%@ does not fit in %@, which is what this parameter counts in"_v,
                                  written.source, value, typeText(context, decl(), annotation));
        return nullptr;
    }

    return program.types.constType(value, annotation);
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
Constant Resolver::resolveConstant(ast::Expr& expr, DeclPtr<Type> hint) {
    // A literal's kind is `Lit` plus the literal kind, so it is a range rather than one label -
    // see `ast::Expr::Lit`, which the AST requires to be last for exactly this.
    if(expr.kind >= ast::Expr::Lit) return resolveLiteral(expr, hint, false);

    switch(expr.kind) {
        case ast::Expr::Coerce: {
            auto& coerce = *parse[expr.coerce];
            auto type = resolveType(coerce.type);
            if(!type) return Constant();

            return resolveConstant(coerce.target, type);
        }
        case ast::Expr::Nested:
            return resolveConstant(*parse[expr.nested], hint);
        case ast::Expr::Con:
            return resolveBoolean(expr, hint);

        /*
         * `-1` is a written negative number, not a negation of a positive one.
         *
         * The lexer produces the magnitude alone, so folding the sign in here is what makes
         * `I64`'s own minimum expressible: negating it after the fact is signed overflow, and
         * `resolve/const.cpp` records that having been the bug on the other side.
         */
        case ast::Expr::Prefix: {
            auto& prefix = *parse[expr.prefix];
            if(prefix.op.kind == ast::Expr::Var &&
               stringView(context.findName(prefix.op.var)) == "-"_v &&
               prefix.on.kind >= ast::Expr::Lit) {
                return resolveLiteral(prefix.on, hint, true);
            }

            unsupported(expr.source, "this constant expression"_v);
            return Constant();
        }
        default:
            unsupported(expr.source, "this constant expression"_v);
            return Constant();
    }
}

/*
 * Whether a constructor expression is one of the two the `Bool` type has.
 *
 * `True` and `False` are nullary constructors rather than literal tokens - see `ast::Literal::Bool`,
 * which the lexer never produces. Core declares them; this stage has the type builtin and knows the
 * two names, which is the same shortcut `Bool` itself is. Asked separately from `resolveBoolean`
 * because a `Con` expression is now also a record construction, and which of the two it is has to be
 * decided before either is attempted.
 */
bool Resolver::isBooleanConstructor(ast::Expr& expr) {
    auto& written = *parse[expr.con];
    if(written.args.size() || written.type.kind != ast::Type::Con) return false;

    auto text = stringView(context.findName(written.type.name));
    return text == "True"_v || text == "False"_v;
}

Constant Resolver::resolveBoolean(ast::Expr& expr, DeclPtr<Type> hint) {
    if(!isBooleanConstructor(expr)) {
        unsupported(expr.source, "this constant expression"_v);
        return Constant();
    }

    auto text = stringView(context.findName(parse[expr.con]->type.name));

    if(hint && decl()[hint]->kind != Type::Bool) {
        context.diagnostics.error("%@ is a Bool, and this position asks for something else"_v,
                                  expr.source, text);
        return Constant();
    }

    return boolConstant(program.types.boolean, text == "True"_v);
}

Constant Resolver::resolveLiteral(ast::Expr& expr, DeclPtr<Type> hint, bool negative) {
    auto type = hint;
    auto& literal = expr.lit;

    switch(ast::Literal::Kind(expr.kind - ast::Expr::Lit)) {
        case ast::Literal::Int: {
            // The default width where nothing asked for one. `I32` rather than the widest,
            // because that is what Core's `Int` is an alias for, and a literal that silently
            // widened would make two programs out of one source.
            if(!type) type = program.types.intType(32, true);

            if(decl()[type]->kind != Type::Int) {
                context.diagnostics.error("an integer literal cannot have this type"_v, expr.source);
                return Constant();
            }

            auto value = literal.i();
            return intConstant(type, negative ? U64(0) - value : value);
        }
        case ast::Literal::Double:
        case ast::Literal::Float: {
            if(!type) type = program.types.floatType(64);

            if(decl()[type]->kind != Type::Float) {
                context.diagnostics.error("a floating-point literal cannot have this type"_v, expr.source);
                return Constant();
            }

            auto value = literal.d();
            return floatConstant(type, negative ? -value : value);
        }
        default:
            unsupported(expr.source, "this literal"_v);
            return Constant();
    }
}

/*
 * A name in scope, innermost first, down to the floor.
 *
 * The floor is zero in every ordinary body and is the whole of what a flattened lens needs from
 * scoping - see `LensFrame`. A callee's body is resolved inside the caller's walk and must not be
 * able to name the caller's locals, so the backward walk stops rather than continuing into a scope
 * that belongs to a different declaration.
 */
Local* Resolver::findLocal(StringId name) {
    for(Size i = bindings.size(); i > bindingFloor; i--) {
        if(bindings[i - 1].name == name) return bindings[i - 1].local;
    }

    return nullptr;
}

bool Resolver::checkType(Node& node, DeclPtr<Type> expected, LocationId source, StringView what) {
    if(sameType(valueType(node), expected)) return true;

    context.diagnostics.error("%@ must be %@, and this is %@"_v, source, what,
                              typeText(context, decl(), expected),
                              typeText(context, decl(), valueType(node)));
    return false;
}

/*
 * The order the passes run in, which is not the order they were written in and is load-bearing at
 * three points.
 *
 *  - **Types before anything that names one.** A `data` may hold a record declared below it, so the
 *    shells exist before any field type is resolved.
 *  - **Every declaration before `classifyTypes`.** Whether a type has a teardown depends on the
 *    instances the program wrote, and every read of a name in every body asks that question through
 *    `readMode`. A classification run before the last `instance` would answer differently for the
 *    bodies resolved before it.
 *  - **Every initializer before any body.** A `let`'s type is what its initializer says, and a body
 *    that reads the name needs it. One interleaved walk resolved them in source order, so a function
 *    above the `let` it read was resolved against a global with no type at all.
 */
Ptr<Program> resolveProgram(Context& context, ast::ModuleGroup& root) {
    auto program = Ptr<Program>(new Program());
    auto module = program->addModule(root.name);

    Resolver resolver { context, *program, *context.parseRegion, *module };

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
    for(auto file: root.files) resolver.declareTypes(*file);
    for(auto file: root.files) resolver.declareAliases(*file);

    // The class heads, before the fields: a field type cannot name a class, but a *member* signature
    // can name any class and an `instance` head names one, so every class exists before either.
    for(auto file: root.files) resolver.declareClasses(*file);
    resolver.synthesizeCoreClasses();

    for(auto file: root.files) resolver.declareRecordFields(*file);

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

    for(auto file: root.files) resolver.declareClassMembers(*file);
    for(auto file: root.files) resolver.declare(*file);

    if(context.diagnostics.errorCount()) return program;
    if(!classifyTypes(context, *program)) return program;

    for(auto file: root.files) resolver.resolveInitializers(*file);
    for(auto file: root.files) resolver.resolveBodies(*file);

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

    /*
     * Where the program starts.
     *
     * `main` where the module declares one. Once top-level statements are implemented this becomes a
     * synthesized entry that runs them and then calls `main`, for the reason the current compiler
     * has one: a program with initialization that started at `main` would start after its own
     * initialization had been skipped.
     */
    auto name = context.addUnqualifiedName("main", 4);
    auto modulePointer = module - *program->decl;

        // Arity zero: a program's entry takes no arguments, and a `main` that took some is a
    // function of that name rather than the entry point - which the "no entry point" message below
    // is the right thing to say about.
    if(auto found = program->findFunction(modulePointer, name, 0)) {
        // A lens answers what its continuation answered, and a program's entry has no caller to
        // supply one. Reported here rather than left to "there is no entry point", which is what a
        // module with no `main` at all is told and is not what happened.
        if((*program->decl)[found]->lens) {
            context.diagnostics.error(
                "a lens answers what its continuation answered, so it cannot be a program's entry point"_v,
                (*program->decl)[found]->source);
        } else {
            program->entry = found;
        }
    }

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

Ptr<Program> resolveProgram(Context& context, ast::Module& root) {
    ast::ModuleGroup group { root.name };
    group.files.push(&root);
    return resolveProgram(context, group);
}

} // namespace mir
