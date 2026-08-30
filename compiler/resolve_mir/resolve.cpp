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
            if(auto record = program.findRecord(here(), type.name)) return asBase<Type>(record);

            context.diagnostics.error("unknown type %@"_v, type.source, context.findName(type.name));
            return nullptr;
        }
        case ast::Type::Ptr: {
            auto to = resolveType(*parse[type.to]);
            return to ? program.types.pointerType(to) : nullptr;
        }
        default:
            unsupported(type.source, "this type form"_v);
            return nullptr;
    }
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

Local* Resolver::findLocal(StringId name) {
    for(Size i = bindings.size(); i > 0; i--) {
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

    for(auto file: root.files) resolver.declareTypes(*file);
    for(auto file: root.files) resolver.declare(*file);

    if(context.diagnostics.errorCount()) return program;
    if(!classifyTypes(context, *program)) return program;

    for(auto file: root.files) resolver.resolveInitializers(*file);
    for(auto file: root.files) resolver.resolveBodies(*file);

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
    if(auto found = program->findFunction(modulePointer, name)) program->entry = found;

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

    computeSummaries(context, *program);
    return program;
}

Ptr<Program> resolveProgram(Context& context, ast::Module& root) {
    ast::ModuleGroup group { root.name };
    group.files.push(&root);
    return resolveProgram(context, group);
}

} // namespace mir
