#include "expr.h"
#include "../parse/ast.h"

/*
 * Handles resolving of patterns.
 * Patterns are resolved recursively by generating code for a pivot (the base structure in the pattern),
 * and then generating code checking if each element in the pivot matches (which may create new pivots).
 * The end result of the match is a boolean indicating if it matched,
 * but any names introduced can be used by the succeeding block (the block that runs if each part matched).
 * For example, the pattern `Just {x = 1, y = 2}` in a match on `a` would generate the following:
 *
 *   %0 = getfield 0, %a : i32
 *   %1 = icmp %0, 0      : i1
 *   je %1, #1, #fail    : void
 * #1:
 *   %2 = getfield 1, 0, %a : i32
 *   %3 = icmp %2, 1        : i1
 *   je %3, #2, #fail       : void
 * #2:
 *   %4 = getfield 1, 1, %a : i32
 *   %5 = icmp %4, 2        : i1
 *   <%5 now contains the match result. It is usually first checked with another je to #fail>
 *
 * #fail:
 *   <rest of match expression here>
 */

auto eqHash = Context::nameHash("==", 2);
auto geHash = Context::nameHash(">=", 2);
auto leHash = Context::nameHash("<=", 2);

Value* resolveVarPat(FunBuilder* b, Value* pivot, ast::VarPat* pat) {
    auto var = findVar(b, pat->var);
    if(var) {
        ast::VarExpr cmp(pat->var);
        List<ast::TupArg> arg(ast::TupArg(0, &cmp));
        auto call = resolveStaticCall(b, eqHash, pivot, &arg, 0);
        if(!call || call->type != &intTypes[IntType::Bool]) {
            if(!call || call->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        }

        return call;
    } else {
        b->block->namedValues[pat->var] = pivot;
        return constInt(b->block, 0, 1, &intTypes[IntType::Bool]);
    }
}

Value* resolveLitPat(FunBuilder* b, Value* pivot, ast::LitPat* pat) {
    ast::LitExpr lit(pat->lit);
    List<ast::TupArg> arg(ast::TupArg(0, &lit));
    auto call = resolveStaticCall(b, eqHash, pivot, &arg, 0);
    if(!call || call->type != &intTypes[IntType::Bool]) {
        if(!call || call->type->kind != Type::Error) {
            error(b, "result of a comparison must be a boolean", pat);
        }
        return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
    }

    return call;
}

Value* resolveAnyPat(FunBuilder* b, Value* pivot, ast::Pat* pat) {
    return constInt(b->block, 0, 1, &intTypes[IntType::Bool]);
}

Value* resolveTupPat(FunBuilder* b, Value* pivot, ast::TupPat* pat) {
    return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
}

Value* resolveConPat(FunBuilder* b, Value* pivot, ast::ConPat* pat) {
    auto con = findCon(&b->context, b->fun->module, pat->constructor);
    if(!compareTypes(&b->context, con->parent, pivot->type)) {
        error(b, "constructor type is incompatible with pivot type", pat);
        return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
    }

    auto intType = &intTypes[IntType::Int];
    auto indices = (U32*)b->mem.alloc(sizeof(U32) * 1);
    indices[0] = 0;

    Value* pivotCon;
    if(pivot->type->kind == Type::Ref) {
        pivotCon = loadField(b->block, 0, pivot, intType, indices, 1);
    } else {
        pivotCon = getField(b->block, 0, pivot, intType, indices, 1);
    }

    auto equal = icmp(b->block, 0, pivotCon, constInt(b->block, 0, con->index, intType), ICmp::eq);
    return equal;
}

Value* resolveArrPat(FunBuilder* b, Value* pivot, ast::ArrayPat* pat) {
    return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
}

Value* resolveRestPat(FunBuilder* b, Value* pivot, ast::RestPat* pat) {
    // Currently only used as part of an array pat.
    return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
}

static Value* getRangeArg(FunBuilder* b, ast::Pat* pat) {
    if(pat->kind == ast::Pat::Any) {
        return nullptr;
    } else if(pat->kind == ast::Pat::Var) {
        auto var = findVar(b, ((ast::VarPat*)pat)->var);
        if(var) {
            auto val = useValue(b, var, false);
            auto type = canonicalType(val->type);
            if(type->kind == Type::Int || type->kind == Type::Float) {
                return val;
            }
        }
    } else if(pat->kind == ast::Pat::Lit) {
        auto value = resolveLit(b, &((ast::LitPat*)pat)->lit, 0);
        if(value->type->kind == Type::Int || value->type->kind == Type::Float) {
            return value;
        }
    }

    error(b, "range patterns must use a variable or numeric literal", pat);
    return nullptr;
}

Value* resolveRangePat(FunBuilder* b, Value* pivot, ast::RangePat* pat) {
    auto from = getRangeArg(b, pat->from);
    auto to = getRangeArg(b, pat->to);

    if(!from && !to) {
        return constInt(b->block, 0, 1, &intTypes[IntType::Bool]);
    } else if(!from) {
        auto fromArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        fromArgs[0] = pivot;
        fromArgs[1] = to;
        auto fromCmp = genStaticCall(b, leHash, fromArgs, 2, 0);

        if(!fromCmp || fromCmp->type != &intTypes[IntType::Bool]) {
            if(!fromCmp || fromCmp->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        }

        return fromCmp;
    } else if(!to) {
        auto toArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        toArgs[0] = pivot;
        toArgs[1] = from;
        auto toCmp = genStaticCall(b, geHash, toArgs, 2, 0);

        if(!toCmp || toCmp->type != &intTypes[IntType::Bool]) {
            if(!toCmp || toCmp->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        }

        return toCmp;
    } else {
        auto fromArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        fromArgs[0] = pivot;
        fromArgs[1] = from;
        auto fromCmp = genStaticCall(b, geHash, fromArgs, 2, 0);

        auto toArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        toArgs[0] = pivot;
        toArgs[1] = to;
        auto toCmp = genStaticCall(b, leHash, toArgs, 2, 0);

        if(!fromCmp || !toCmp || fromCmp->type != &intTypes[IntType::Bool] || toCmp->type != &intTypes[IntType::Bool]) {
            if(!fromCmp || !toCmp || fromCmp->type->kind != Type::Error || toCmp->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        }

        return and_(b->block, 0, fromCmp, toCmp);
    }
}

Value* resolvePat(FunBuilder* b, Value* pivot, ast::Pat* pat) {
    switch(pat->kind) {
        case ast::Pat::Error:
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
        case ast::Pat::Var:
            return resolveVarPat(b, pivot, (ast::VarPat*)pat);
        case ast::Pat::Lit:
            return resolveLitPat(b, pivot, (ast::LitPat*)pat);
        case ast::Pat::Any:
            return resolveAnyPat(b, pivot, pat);
        case ast::Pat::Tup:
            return resolveTupPat(b, pivot, (ast::TupPat*)pat);
        case ast::Pat::Con:
            return resolveConPat(b, pivot, (ast::ConPat*)pat);
        case ast::Pat::Array:
            return resolveArrPat(b, pivot, (ast::ArrayPat*)pat);
        case ast::Pat::Rest:
            return resolveRestPat(b, pivot, (ast::RestPat*)pat);
        case ast::Pat::Range:
            return resolveRangePat(b, pivot, (ast::RangePat*)pat);
        default:
            return constInt(b->block, 0, 0, &intTypes[IntType::Bool]);
    }
}