/*
 * Signatures: what a function takes and what it hands back.
 *
 * Separate from the body it belongs to, because every signature in a module is resolved before any
 * body is - a call in one body names a function declared in another, and overload resolution needs
 * all of them. The return-root rules are here for the same reason they are a signature's business
 * at all: which arguments a returned borrow may have come from is part of the contract a caller
 * compiles against.
 */

#include "module_internal.h"
#include "analyze.h"
#include "const.h"
#include "core.h"
#include "expr.h"
#include "generic.h"
#include "host.h"
#include "index.h"
#include "name.h"
#include "native.h"
#include "verify.h"
#include "witness.h"
#include "../parse/ast.h"

/*
 * What makes a returned borrow exclusive.
 *
 * `&T` in type position says a borrow and not which kind, because the answer is not a property
 * of the result: Design.md's rule is that "a returned mutable borrow must be rooted in a
 * `return &` mutable parameter", so the result is exclusive exactly when every member of the
 * group it may be rooted in is. A mixed group yields the weaker capability, which is the same
 * rule read the other way - an immutable result may be rooted in either kind.
 *
 * Split out of resolveSignature() because an inferred result reaches it a pass later, once the body
 * has said what the type is - the rule is the same either way, only the timing differs.
 */
void applyReturnRoots(Module& module, Function& function, LocationId source) {
    if(!isBorrow(*module.types, function.returnType)) return;

    if(!function.returnRoots) {
        if(!function.returnRootWritten) {
            module.context.diagnostics.error("a function returning a borrow must mark the argument it is rooted in with `return`"_v,
                                             source);
        }
    } else {
        function.returnType = applyReturnRootMutability(module, function.returnType, function.returnRootsMutable);
    }
}

TypePtr requireReturnType(Module& module, Function& function, LocationId source) {
    if(function.returnType) return function.returnType;
    if(!function.inferReturn) return function.returnType;

    /*
     * The body is already on the stack, so the type it is about to produce is the type being asked
     * for. Only an explicit result type breaks the cycle, which is what this says.
     *
     * The error type rather than unit, and recorded rather than returned: recorded so that a second
     * caller does not report the same cycle again, and the error type because unit is a type that
     * type-checks. A recursive `fact` recorded as unit goes on to report that `*` does not accept
     * `(Int, ())`, which is a second diagnostic about a signature the author never wrote.
     */
    if(function.resolving) {
        module.context.diagnostics.error("%@ is recursive, so the `=` form cannot infer its result type from its body - write it out, as in `-> Int`"_v,
                                         source, module.context.findName(function.name));
        function.inferReturn = false;
        function.returnType = module.scalar.error;
        return function.returnType;
    }

    resolveFunctionBody(module, function);

    // A body that resolved without producing one - it reported why, and the error type keeps that
    // one diagnostic from turning into a null dereference in whatever asked.
    if(!function.returnType) function.returnType = module.scalar.error;

    return function.returnType;
}

/*
 * `= expr` on a parameter: the constant a call site that leaves the position out passes instead.
 *
 * A constant and not an expression, which is the rule a field default is already under and is stated
 * once for both - see const.h. So what is recorded is the same thing: the constant the parameter
 * starts at, which the call site turns back into a value with `constantValue`. Nothing is evaluated
 * at the call site, and nothing has to run at the declaration.
 *
 * Three positions cannot have one, and each is a rule about the *caller* rather than about the
 * constant. A `return` parameter roots borrows in the result, and an omitted one has no caller
 * storage for a result borrow to stay live in - doc/spec/functions.md says so directly. A `&`
 * parameter is written through, and a temporary built from a constant is somewhere the caller never
 * looks again. And a parameter declared as a type variable has no type for the constant to *be* of:
 * which type it would be is exactly what each call site decides separately.
 */
static void resolveArgumentDefault(Module& module, Arg& declared, const ast::Expr& expr, LocationId source) {
    auto global = *module.types;

    if(declared.returnRoot) {
        module.context.diagnostics.error("a `return` parameter cannot have a default value - the marker says a borrow in the result may be rooted in this argument, and a call site that leaves it out has no storage of its own for the result to stay live in"_v,
                                         source);
        return;
    }

    if(declared.isMutableBorrow()) {
        module.context.diagnostics.error("a `&` parameter cannot have a default value - the callee writes through it, and a call site that left it out would be handed a temporary built from the constant, which nothing reads afterwards"_v,
                                         source);
        return;
    }

    if(isGeneric(global, declared.type)) {
        module.context.diagnostics.error("a parameter declared as a type variable cannot have a default value - a default is a constant of the parameter's type, and which type this one has is what each call site decides"_v,
                                         source);
        return;
    }

    if(global[declared.type]->kind == Type::Error) return;

    auto constant = evaluateConstant(module, expr, declared.type, "a default argument"_v, false);
    if(constant) declared.defaultValue = constant;
}

/*
 * Resolves one function signature against a generic context, producing a body-less Function.
 *
 * `classSignature` says this is a class member rather than something callable, and the whole of what
 * it changes is that a `lens fn` or `iter fn` member is *not* desugared here - see
 * Implementation-Containers.md §5, whose `Chunked` declares `iter fn chunks`.
 *
 * The desugaring introduces a type variable for the continuation's result, and a class member has
 * nowhere to put one: a class's variables are its head's, instance selection binds them by index,
 * and a member signature holding a variable the head does not declare would be a position no
 * selection could ever fill. So what a class declares is the written shape - the arguments an
 * instance takes and the type it hands over - and each implementation desugars against a context of
 * its own, which is where the continuation belongs anyway since that is where the `yield` is.
 */
Function* resolveSignature(Module& module, ast::Decl& decl, GenEnv* env, StringId name, bool anonymous,
                                  bool classSignature) {
    auto function = anonymous ? addAnonymousFunction(module, name, decl.source)
                              : module.addFunction(name, decl.source);

    function->funKind = decl.fun.kind;

    /*
     * An `=` function that wrote no result type has one anyway - the type of its body.
     *
     * Leaving it null and letting resolveFunctionBody() fill it in is what makes `fn sum(a, b) = a + b`
     * return its sum instead of computing it and discarding it. Defaulting to unit is right only for
     * the block form, where falling off the end really does produce nothing.
     */
    if(!decl.fun.ret && decl.fun.implicitReturn && decl.fun.kind == ast::FunKind::Plain) {
        function->inferReturn = true;
        function->returnType = nullptr;
    } else {
        function->returnType = decl.fun.ret ? resolveType(module, *module.parse[decl.fun.ret], env)
                                            : module.scalar.unit;
    }

    U16 index = 0;
    auto roots = 0u;

    // Markers that were written, valid or not. A signature whose only marker was rejected has
    // already been told what is wrong with it, and "you must mark an argument" would be the second
    // diagnostic about the same line saying the opposite of the first.
    auto written = 0u;
    auto allRootsMutable = true;

    for(auto arg: decl.fun.args.contents(module.parse)) {
        if(!arg.type) {
            module.context.diagnostics.error("function arguments require an explicit type"_v, arg.source);
            function->addArg(module, arg.name, module.scalar.error, arg.source);
            index++;
            continue;
        }

        /*
         * A value parameter with a const parameter's name - Implementation-Const-Generics.md §1.6.
         *
         * `fn (n: Int) f(n: Int)` has two things called `n` in one signature and no reading of it is
         * the obvious one: the body's `n` would be the argument, and the count in `[Int *n]` beside
         * it would be the parameter, so the same name would mean two numbers a line apart. Reported
         * at the declaration rather than resolved by shadowing.
         */
        if(env && arg.name) {
            if(auto clash = findGenVariable(module, *env, arg.name)) {
                if((*module.types)[clash]->kind == GenKind::Const) {
                    module.context.diagnostics.error("%@ is already a const parameter of this signature, so it cannot also be an argument - the two would be different numbers under one name"_v,
                                                     arg.source, module.context.findName(arg.name));
                }
            }
        }

        auto type = bindingType(module, *module.parse[arg.type], arg.bind, env);
        auto lazy = arg.lazy && checkLazyArgument(module, arg.bind, arg.returnRoot, arg.source);

        /*
         * A `@lazy` parameter of a lens or an iterator, which this version does not build.
         *
         * Reported rather than accepted, because accepting it was silently wrong: a lens or
         * iterator call resolves its written arguments *before* it lifts the continuation - see
         * resolveHandedArguments - and the thunk was then made out of the value that resolving
         * already produced. So the argument ran whether or not the callee forced it, which is the
         * one thing `@lazy` promises it does not do, and nothing anywhere said so.
         *
         * Not a small omission to close, which is why it is a rule for now rather than a gap.
         * `continuationSignature` infers the continuation's own parameter types by binding the
         * callee's type variables against the argument *values*, and a position that is deferred has
         * no value to bind - so deferring one here means teaching the continuation's shape to be
         * inferred from a promise. Design.md's uses of `@lazy` are all short-circuiting operators,
         * none of which is a lens or an iterator.
         */
        if(lazy && function->funKind != ast::FunKind::Plain) {
            module.context.diagnostics.error(function->funKind == ast::FunKind::Iter
                ? "an `iter fn` cannot declare a `@lazy` parameter - a `for` loop resolves the arguments before it lifts its body into the continuation, so the argument would run whether or not the iterator forced it. Take a function and call it, which is what the thunk would have been"_v
                : "a `lens fn` cannot declare a `@lazy` parameter - the call site resolves the arguments before it lifts the rest of the block into the continuation, so the argument would run whether or not the lens forced it. Take a function and call it, which is what the thunk would have been"_v,
                arg.source);

            lazy = false;
        }

        // A `@lazy` parameter arrives as the thunk rather than as the value, so that is the type
        // the parameter has; what the signature promised is kept beside it - see Arg::lazyType.
        auto declared = function->addArg(module, arg.name, lazy ? resolveThunkType(module, type) : type,
                                         arg.source);
        declared->convention = arg.bind;
        declared->returnRoot = arg.returnRoot;

        if(lazy) {
            declared->lazyType = type;

            // What every call site of this name consults before it evaluates anything - see
            // Program::lazyNames.
            module.program.lazyNames.add(name);
        }

        if(lazy && arg.def) {
            module.context.diagnostics.error("a `@lazy` parameter cannot have a default value - the default would be one more expression the call site did not write and the callee may not run"_v,
                                             arg.source);
        } else if(arg.def) {
            resolveArgumentDefault(module, *declared, *module.parse[arg.def], arg.source);
        }

        // What may carry the marker is one rule shared with a written function type - see
        // checkReturnRoot, which is where it and its diagnostics live.
        if(arg.returnRoot) {
            written++;

            if(checkReturnRoot(module, type, arg.bind, index, arg.source)) {
                roots++;
                if(arg.bind != ast::BindType::Ref) allRootsMutable = false;
            } else {
                declared->returnRoot = false;
            }
        }

        index++;
    }

    // Before the return-root check below, because it is what decides what this function returns: a
    // lens returns its continuation's result and an iterator the step signal, neither of which is a
    // borrow, and the values either hands over are bounded by that continuation rather than by a
    // return edge - Implementation-Lens.md part 2's "a lens callback is exempt".
    if(function->funKind != ast::FunKind::Plain && !classSignature) {
        resolveLensSignature(module, *function, env, decl);
    } else if(decl.fun.retBind != ast::BindType::Borrow) {
        /*
         * `-> ->T` on a function that hands nothing over - Analysis-Language.md §3a.
         *
         * A result convention says how the *receiver* of a value binds it, and an ordinary function
         * has no receiver to say it about: what a `return` produces is already the caller's, and
         * there is no second binding site for a marker to describe. Only a `lens` and an `iter`
         * have one, which is their continuation.
         */
        module.context.diagnostics.error("`-> ->` says how what this hands over is received, and only a `lens fn` or an `iter fn` hands anything over - an ordinary function's result is the caller's already"_v,
                                         decl.source);
    }

    function->returnRoots = roots > 0;
    function->returnRootWritten = written > 0;
    function->returnRootsMutable = allRootsMutable;

    // A function still waiting on its body has no result type to check yet. The same check runs
    // from resolveFunctionBody() once there is one, off the three flags just recorded.
    if(!function->inferReturn) applyReturnRoots(module, *function, decl.source);

    return function;
}
