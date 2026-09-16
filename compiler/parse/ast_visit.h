#pragma once

#include "ast.h"

namespace ast {

/*
 * Every expression written *inside* one, in source order.
 *
 * The AST's shape, stated once. Anything asking a question of the form "does this statement do X
 * anywhere in it" needs the list of children of every kind of node, and the list is long enough that
 * a second copy of it drifts: the reason `Expr::Field` holds the field *name* as an expression, or
 * that a `for` has two optional bounds, is not something a reader of a second walk would reconstruct.
 *
 * `-Wswitch` is what keeps it current - the switch below has no `default:`, so a new `Expr::Kind`
 * fails the build here rather than being silently skipped by whoever is walking. That is the same
 * enforcement `resolve_mir/inst.def`'s switches rely on, and it is why this is a switch over every
 * kind rather than a table of the interesting ones.
 *
 * The walk is one level deep: `visit` is called with each direct child, and a caller that wants the
 * whole subtree recurses from there. That is what lets a caller stop at a node whose children mean
 * something else - a field's name is a name and not a use of one, which is the one case that has
 * come up - without this file knowing what the question was.
 */
template<class F>
void visitChildren(ParseBase base, Expr& expr, F&& visit) {
    // A literal has no children, and its kind is out of the enum's range - the literal's own type
    // is `kind - Expr::Lit`, so a switch cannot see it.
    if(expr.kind >= Expr::Lit) return;

    auto each = [&](ParseList<Expr> list) {
        for(auto child: list.contents(base)) visit(child);
    };

    auto arguments = [&](ParseList<TupArg> list) {
        for(auto argument: list.contents(base)) visit(argument.value);
    };

    auto maybe = [&](ParsePtr<Expr> child) {
        if(child) visit(*base[child]);
    };

    switch(expr.kind) {
        case Expr::Error:
        case Expr::Var:
        case Expr::Lit:
            return;

        case Expr::Multi:
            each(expr.multi);
            return;
        case Expr::Nested:
            visit(*base[expr.nested]);
            return;
        case Expr::App:
        case Expr::Sub: {
            auto& application = *base[expr.kind == Expr::App ? expr.app : expr.sub];
            visit(application.callee);
            arguments(application.args);
            return;
        }
        case Expr::Fun: {
            auto& lambda = *base[expr.fun];
            for(auto argument: lambda.args.contents(base)) maybe(argument.def);
            visit(lambda.body);
            return;
        }
        case Expr::Infix: {
            auto& infix = *base[expr.infix];
            visit(infix.op);
            visit(infix.lhs);
            visit(infix.rhs);
            return;
        }
        case Expr::Prefix: {
            auto& prefix = *base[expr.prefix];
            visit(prefix.op);
            visit(prefix.on);
            return;
        }
        case Expr::If: {
            auto& branch = *base[expr.singleIf];
            visit(branch.cond);
            visit(branch.then);
            if(branch.otherwise) visit(branch.otherwise.unwrap());
            return;
        }
        case Expr::MultiIf:
            for(auto branch: expr.multiIf.contents(base)) {
                visit(branch.cond);
                visit(branch.then);
            }
            return;
        case Expr::Decl:
            for(auto binding: expr.decl.contents(base)) {
                maybe(binding.content);
                maybe(binding.in);
                for(auto alternative: binding.alts.contents(base)) visit(alternative.expr);
            }
            return;
        case Expr::While: {
            auto& loop = *base[expr.whileLoop];
            visit(loop.cond);
            visit(loop.body);
            return;
        }
        case Expr::For: {
            auto& loop = *base[expr.forLoop];
            visit(loop.from);
            maybe(loop.to);
            maybe(loop.step);
            for(auto alternative: loop.alts.contents(base)) visit(alternative.expr);
            visit(loop.body);
            return;
        }
        case Expr::Assign: {
            auto& assignment = *base[expr.assign];
            visit(assignment.target);
            visit(assignment.value);
            return;
        }
        case Expr::Coerce:
            visit(base[expr.coerce]->target);
            return;
        case Expr::Field: {
            auto& field = *base[expr.field];
            visit(field.target);

            // The selected field, which `resolveExpr` requires to be a name. Visited like any other
            // child: what it *means* is a caller's question, and the two callers so far want
            // different answers.
            visit(field.field);
            return;
        }
        case Expr::Con:
            arguments(base[expr.con]->args);
            return;
        case Expr::Tup:
            for(auto field: expr.tup.contents(base)) visit(field.value);
            return;
        case Expr::TupUpdate: {
            auto& update = *base[expr.tupUpdate];
            visit(update.value);
            for(auto field: update.args.contents(base)) visit(field.value);
            return;
        }
        case Expr::Array:
            each(expr.arr);
            return;
        case Expr::Map:
            for(auto entry: expr.map.contents(base)) {
                visit(entry.key);
                visit(entry.value);
            }
            return;
        case Expr::Format:
            for(auto chunk: expr.format.contents(base)) maybe(chunk.format);
            return;
        case Expr::Match: {
            auto& match = *base[expr.match];
            visit(match.pivot);
            for(auto alternative: match.alts.contents(base)) visit(alternative.expr);
            return;
        }
        case Expr::Range: {
            auto& range = *base[expr.range];
            visit(range.from);
            visit(range.to);
            return;
        }
        case Expr::Ret:
            maybe(expr.ret);
            return;
        case Expr::Yield:
            visit(*base[expr.yield]);
            return;
        case Expr::Break:
            maybe(expr.breakValue);
            return;
        case Expr::Continue:
            maybe(expr.continueValue);
            return;
        case Expr::Is:
            visit(base[expr.is]->value);
            return;
        case Expr::Try:
            visit(*base[expr.tryValue]);
            return;
        case Expr::Unwrap:
            visit(*base[expr.unwrap]);
            return;
        case Expr::Dispose:
            visit(*base[expr.dispose]);
            return;
    }
}

} // namespace ast
