/*
 * Class defaults: a body the class writes for one of its own signatures.
 *
 * Two things, and the second is the interesting one. A default body is resolved once against the
 * class's own variables and specialized per instance that inherited it, which is what
 * resolveClassDefault does. The rest is the order they may be written in: a default may call
 * another default, so the class's members form a graph, and a cycle in it is a program that would
 * recurse forever with nothing to report - `rankDefault` is where that is found instead.
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
 * Class default implementations.
 *
 * A default is a body the class writes for one of its own signatures, used by every instance that
 * does not supply that function. What it is, exactly, is a generic function over the class's type
 * variables carrying the class itself as a requirement - `fn (Eq(a)) !=(lhs: a, rhs: a) -> Bool`
 * written inside `class Eq(a)` - which is structurally what a parametric instance's implementation
 * already is. So the body is resolved once against those variables and specialized per instance
 * that inherited it, and nothing about overloading changes: a default is a fallback body for a
 * signature that is already a member of the set, not a new member of it.
 *
 * A default body is part of the class's exported contract. Changing one changes behavior for every
 * instance that did not override it, in every module that compiled against the class.
 */

// `Eq.!=`, the name a default is printed and specialized under. It is not addressable in source;
// what it needs is to be unique and to say where it came from.
static StringId classDefaultName(Module& module, TypeClass& typeClass, StringId method) {
    StringBuilder text;
    text << module.context.findName(typeClass.name) << '.' << module.context.findName(method);
    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

ModulePtr<Function> resolveClassDefault(Module& module, TypeClass& typeClass, ast::Decl& member,
                                               ast::ParsePtr<ast::Decl> pointer, Function& signature) {
    auto global = *module.types;
    auto classEnv = global[typeClass.gen];

    // The context holds the class's own variables rather than copies of them, so the signature's
    // types can be reused as they are - substitution and matching both go by variable index, and
    // these are the indices an instance selection binds. Only the requirements differ: the class's
    // superclasses come along, which is what lets `Num`'s unary `-` default to `0 - value` through
    // the `FromInt` its class already declares, and the class itself is added, since that is what a
    // default calling a sibling needs and what pushing onto the class's own context would instead
    // have turned into a further superclass.
    auto env = new (module.types) GenEnv(GenEnv::Function);
    for(auto variable: classEnv->types.contents(global)) env->types.push(module.types, variable);

    auto addConstraint = [&](GlobalPtr<TypeClass> target, StringId name, LocationId source, Buffer<TypePtr> args) {
        ClassConstraint constraint;
        constraint.typeClass = target;
        constraint.name = name;
        constraint.source = source;
        for(auto arg: args) constraint.args.push(module.types, arg);
        env->classes.push(module.types, constraint);
    };

    for(auto constraint: classEnv->classes.contents(global)) {
        TypeList args;
        for(auto arg: constraint.args.contents(global)) args.push(arg);
        addConstraint(constraint.typeClass, constraint.name, constraint.source, toBuffer(args));
    }

    TypeList own;
    for(auto variable: classEnv->types.contents(global)) own.push((Type*)global[variable] - global);
    addConstraint((TypeClass*)&typeClass - global, typeClass.name, member.source, toBuffer(own));

    auto function = addAnonymousFunction(module, classDefaultName(module, typeClass, member.fun.name), member.source);
    function->gen = env - global;
    function->classDefault = true;
    function->returnType = signature.returnType;
    function->ast = pointer;
    readInlineAttribute(module, member, *function);

    for(auto argPointer: signature.args.contents(*module.arena)) {
        auto arg = (*module.arena)[argPointer];
        auto copied = function->addArg(module, arg->name, arg->type, arg->source);
        copied->convention = arg->convention;
        copied->returnRoot = arg->returnRoot;
        copied->lazyType = arg->lazyType;
    }

    return function - *module.arena;
}

/*
 * The rank rule.
 *
 * Haskell's known hazard is a pair of defaults that call each other - `==` as `not (a /= b)` and
 * `/=` as `not (a == b)` - where an instance supplying neither compiles and hangs. The answer here
 * is a check rather than an informal pragma, since the language's bias is one primitive operation
 * per class rather than a choice of which one to implement:
 *
 *   A function with no default has rank 0; a default may only call class functions of strictly
 *   lower rank than its own.
 *
 * Ranks are inferred rather than written, so what the rule asks is that the defaults of one class
 * do not depend on each other in a circle. That is decidable at the declaration, which is where it
 * is decided - before any body has been resolved and so before any of them could be instantiated.
 */

// One name written in call position in a default body, by the key an overload set is arranged by.
struct DefaultCall {
    StringId name {};
    U16 arity = 0;
};

// Inline: this is one syntactic walk per default body, and a default body calls a few names.
using DefaultCallList = SmallArray<DefaultCall, 16>;

static void collectCalls(ast::ParseBase parse, ast::Expr expr, DefaultCallList& target);

static void collectCallee(ast::ParseBase parse, ast::Expr callee, U16 arity, DefaultCallList& target) {
    if(callee.kind == ast::Expr::Var) target.push(DefaultCall { callee.var, arity });
    else collectCalls(parse, callee, target);
}

/*
 * Every name one default body could be calling.
 *
 * Deliberately syntactic and over-approximate. A name in call position counts as a call whether or
 * not selection would have chosen the class function of that name, so a default that shadows a
 * sibling with a plain function of the same name is refused rather than ranked as if it had not
 * called it. Rejecting a declaration that would have worked is a cost; ranking one that hangs is
 * not a cost this check is allowed to have.
 */
static void collectCalls(ast::ParseBase parse, ast::Expr expr, DefaultCallList& target) {
    auto walk = [&](ast::Expr child) { collectCalls(parse, child, target); };
    auto walkPointer = [&](ast::ParsePtr<ast::Expr> child) { if(child) walk(*parse[child]); };
    auto walkArgs = [&](ast::ParseList<ast::TupArg> args) {
        for(auto arg: args.contents(parse)) walk(arg.value);
    };

    switch(expr.kind) {
        case ast::Expr::Multi:
            for(auto child: expr.multi.contents(parse)) walk(child);
            break;
        case ast::Expr::App:
        case ast::Expr::Sub: {
            auto& app = *parse[expr.kind == ast::Expr::App ? expr.app : expr.sub];
            collectCallee(parse, app.callee, U16(app.args.size()), target);
            walkArgs(app.args);
            break;
        }
        case ast::Expr::Fun: {
            auto& fun = *parse[expr.fun];
            for(auto arg: fun.args.contents(parse)) walkPointer(arg.def);
            walk(fun.body);
            break;
        }
        case ast::Expr::Infix: {
            auto& infix = *parse[expr.infix];
            collectCallee(parse, infix.op, 2, target);
            walk(infix.lhs);
            walk(infix.rhs);
            break;
        }
        case ast::Expr::Prefix: {
            auto& prefix = *parse[expr.prefix];
            collectCallee(parse, prefix.op, 1, target);
            walk(prefix.on);
            break;
        }
        case ast::Expr::If: {
            auto& branch = *parse[expr.singleIf];
            walk(branch.cond);
            walk(branch.then);
            if(branch.otherwise) walk(branch.otherwise.unwrap());
            break;
        }
        case ast::Expr::MultiIf:
            for(auto branch: expr.multiIf.contents(parse)) {
                walk(branch.cond);
                walk(branch.then);
            }
            break;
        case ast::Expr::Decl:
            for(auto var: expr.decl.contents(parse)) {
                walkPointer(var.content);
                walkPointer(var.in);
                for(auto alt: var.alts.contents(parse)) walk(alt.expr);
            }
            break;
        case ast::Expr::While: {
            auto& loop = *parse[expr.whileLoop];
            walk(loop.cond);
            walk(loop.body);
            break;
        }
        case ast::Expr::For: {
            auto& loop = *parse[expr.forLoop];
            walk(loop.from);
            walkPointer(loop.to);
            walkPointer(loop.step);
            walk(loop.body);
            break;
        }
        case ast::Expr::Assign: {
            auto& assign = *parse[expr.assign];
            walk(assign.target);
            walk(assign.value);
            break;
        }
        case ast::Expr::Nested:
            walkPointer(expr.nested);
            break;
        case ast::Expr::Coerce:
            walk(parse[expr.coerce]->target);
            break;
        case ast::Expr::Field: {
            auto& field = *parse[expr.field];
            walk(field.target);
            walk(field.field);
            break;
        }
        case ast::Expr::Con:
            walkArgs(parse[expr.con]->args);
            break;
        case ast::Expr::Tup:
            walkArgs(expr.tup);
            break;
        case ast::Expr::TupUpdate: {
            auto& update = *parse[expr.tupUpdate];
            walk(update.value);
            for(auto arg: update.args.contents(parse)) walk(arg.value);
            break;
        }
        case ast::Expr::Array:
            for(auto child: expr.arr.contents(parse)) walk(child);
            break;
        case ast::Expr::Map:
            for(auto entry: expr.map.contents(parse)) {
                walk(entry.key);
                walk(entry.value);
            }
            break;
        case ast::Expr::Format:
            for(auto chunk: expr.format.contents(parse)) walkPointer(chunk.format);
            break;
        case ast::Expr::Match: {
            auto& match = *parse[expr.match];
            walk(match.pivot);
            for(auto alt: match.alts.contents(parse)) walk(alt.expr);
            break;
        }
        case ast::Expr::Range: {
            auto& range = *parse[expr.range];
            walk(range.from);
            walk(range.to);
            break;
        }
        case ast::Expr::Ret:
            walkPointer(expr.ret);
            break;
        case ast::Expr::Yield:
            walkPointer(expr.yield);
            break;
        case ast::Expr::Break:
            walkPointer(expr.breakValue);
            break;
        case ast::Expr::Is:
            walk(parse[expr.is]->value);
            break;
        case ast::Expr::Try:
            // The `Try` calls `?` itself makes are Core's and are reached by class rather than by
            // name, so there is no callee here to collect - only the operand's own calls.
            walkPointer(expr.tryValue);
            break;
        case ast::Expr::Unwrap:
            walkPointer(expr.unwrap);
            break;
        default:
            // Error, Var, Continue and the literals call nothing. A bare name is not a call today:
            // function values are rejected by the resolver, so a class function's name can only
            // reach it from one of the positions above.
            break;
    }
}

// Ranks one default, depth first, reporting the circle if its body leads back to a default that is
// still being ranked. A default that takes part in one loses it, so that what follows is an
// instance that has to write the function rather than a compiler that instantiates forever.
static void rankDefault(Module& module, TypeClass& typeClass, Size index, SmallArray<U8, 8>& state) {
    auto global = *module.types;
    auto entry = typeClass.functions.get(global, index);
    if(state[index] || !entry.defaultFun) return;

    state[index] = 1;

    auto& decl = *module.parse[(*module.arena)[entry.defaultFun]->ast];
    DefaultCallList calls;
    if(decl.fun.body) collectCalls(module.parse, *module.parse[decl.fun.body], calls);

    U16 rank = 1;
    auto circular = false;

    for(auto& call: calls) {
        for(Size other = 0; other < typeClass.functions.size(); other++) {
            auto called = typeClass.functions.get(global, other);
            if(called.name != call.name || called.arity != call.arity) continue;

            if(state[other] == 1) {
                if(other == index) {
                    module.context.diagnostics.error(
                        "the default for %@ calls %@ - a default may only call class functions of strictly lower rank than its own, and nothing is lower than itself"_v,
                        decl.source, module.context.findName(entry.name), module.context.findName(called.name));
                } else {
                    module.context.diagnostics.error(
                        "the default for %@ calls %@, whose own default leads back to it - an instance supplying neither would have nothing to run. A default may only call class functions of strictly lower rank, so one of the two has to be left for every instance to write"_v,
                        decl.source, module.context.findName(entry.name), module.context.findName(called.name));
                }

                circular = true;
                break;
            }

            rankDefault(module, typeClass, other, state);

            auto ranked = typeClass.functions.get(global, other).rank;
            if(ranked + 1 > rank) rank = U16(ranked + 1);
            break;
        }

        if(circular) break;
    }

    entry.rank = circular ? 0 : rank;
    if(circular) entry.defaultFun = nullptr;

    typeClass.functions.set(global, index, entry);
    state[index] = 2;
}

void checkDefaultRanks(Module& module, TypeClass& typeClass) {
    SmallArray<U8, 8> state;
    for(Size i = 0; i < typeClass.functions.size(); i++) state.push(0);
    for(Size i = 0; i < typeClass.functions.size(); i++) rankDefault(module, typeClass, i, state);
}
