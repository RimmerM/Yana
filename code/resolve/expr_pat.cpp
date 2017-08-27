#include <cstdio>
#include "expr.h"
#include "../parse/ast.h"

/*
 * Handles resolving of patterns.
 * Patterns are resolved recursively by generating code for a pivot (the base structure in the pattern),
 * and then generating code checking if each element in the pivot matches (which may create new pivots).
 * The end result of the match is a non-terminating block that runs if the match succeeds,
 * as well as a non-terminating block that runs on failure. Any names introduced are visible in the succeeding block.
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
 *   je %5, #3, #fail
 * #3:
 *   <returned as succeeding block>
 *
 * #fail:
 *   <returned as failure block>
 */

struct Matcher {
    bool alwaysTrue = false;
    bool alwaysFalse = false;
    Value* comparison = nullptr;
};

auto eqHash = Context::nameHash("==", 2);
auto geHash = Context::nameHash(">=", 2);
auto leHash = Context::nameHash("<=", 2);

void resolveVarPat(FunBuilder* b, Matcher* matcher, Value* pivot, ast::VarPat* pat) {
    auto var = findVar(b, pat->var);
    if(var) {
        ast::VarExpr cmp(pat->var);
        List<ast::TupArg> arg(ast::TupArg(0, &cmp));
        auto call = resolveStaticCall(b, eqHash, pivot, &arg, 0);
        if(!call || call->type != &intTypes[IntType::Bool]) {
            if(!call || call->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }

            matcher->alwaysFalse = true;
        } else {
            matcher->comparison = call;
        }
    } else {
        b->block->namedValues[pat->var] = pivot;
        matcher->alwaysTrue = true;
    }
}

void resolveLitPat(FunBuilder* b, Matcher* matcher, Value* pivot, ast::LitPat* pat) {
    ast::LitExpr lit(pat->lit);
    List<ast::TupArg> arg(ast::TupArg(0, &lit));
    auto call = resolveStaticCall(b, eqHash, pivot, &arg, 0);
    if(!call || call->type != &intTypes[IntType::Bool]) {
        if(!call || call->type->kind != Type::Error) {
            error(b, "result of a comparison must be a boolean", pat);
        }
        matcher->alwaysFalse = true;
    } else {
        matcher->comparison = call;
    }
}

int resolveTupPat(FunBuilder* b, MatchContext** match, Block* onFail, Value* pivot, ast::TupPat* pat) {
    auto type = canonicalType(pivot->type);
    if(type->kind != Type::Tup) {
        error(b, "cannot match a tuple on this type", pat);
        jmp(b->block, onFail);
        return -1;
    }

    bool alwaysSucceeds = true;
    auto tup = (TupType*)type;

    U32 i = 0;
    auto field = pat->fields;
    while(field) {
        int index = -1;
        if(field->item.field) {
            for(U32 f = 0; f < tup->count; f++) {
                if(tup->fields[f].name == field->item.field) {
                    index = f;
                    break;
                }
            }
        } else if(i < tup->count) {
            index = i;
        }

        if(index < 0) {
            error(b, "cannot match field: the tuple doesn't contain this field", field->item.pat);
            jmp(b->block, onFail);
            return -1;
        } else {
            auto f = &tup->fields[index];
            auto get = getField(b, pivot, 0, f->index, f->type);

            // Create a matching context for this tuple if none existed.
            MatchContext* context = *match;
            if(!context) {
                context = new (b->exprMem) MatchContext;
                *match = context;
            }

            // Keep track of a linked list of child patterns that match constructors.
            MatchContext** childContext;
            auto p = context->children;
            if(!p || p->childIndex == index) {
                childContext = &context->children;
            } else {
                childContext = nullptr;
                while(p->next) {
                    if(p->next->childIndex == index) {
                        childContext = &p->next;
                        break;
                    }
                    p = p->next;
                }

                if(!childContext) {
                    childContext = &p->next;
                }
            }

            auto result = resolvePat(b, childContext, onFail, get, field->item.pat);

            if(*childContext) {
                (*childContext)->childIndex = index;
            }

            // If the match always fails, the whole pattern fails.
            // If the match may succeed or always succeeds, continue matching on the current block.
            if(result == -1) {
                return -1;
            }

            if(result != 1) {
                alwaysSucceeds = false;
            }
        }

        i++;
        field = field->next;
    }

    return alwaysSucceeds ? 1 : 0;
}

int resolveConPat(FunBuilder* b, MatchContext** match, Block* onFail, Value* pivot, ast::ConPat* pat) {
    auto con = findCon(&b->context, b->fun->module, pat->constructor);
    if(!compareTypes(&b->context, con->parent, pivot->type)) {
        error(b, "constructor type is incompatible with pivot type", pat);
        jmp(b->block, onFail);
        return -1;
    }

    // Constructor patterns are a bit complicated.
    // We first want to know two things:
    //  - Will the child pattern always succeed.
    //  - Is this the last constructor we check in this type.
    // If both of these are true, we don't have to run the comparison at all and can just assume it is constant.
    // The problem is - if these are _not_ true, the child pattern will run in a different block (`then` instead of the current one).
    // It would be really complicated to find all code generated by the pattern and move it to a different block.
    //
    // What we do instead:
    //  - Assume that the comparison is needed, but first generate code for the child in `then`.
    //  - Check if both conditions are true.
    //     - If true, generate a static jmp from the initial block to `then`. This is easy to optimize out later.
    //     - If not, generate code as we would normally.
    auto initial = b->block;
    auto then = block(b->fun, true);
    b->block = then;

    // If no match context exists for this position, create one.
    MatchContext* context = *match;
    if(!context) {
        context = new (b->exprMem) MatchContext;
        context->conCount = con->parent->conCount;
        context->conChecked = (bool*)b->exprMem.alloc(sizeof(bool) * context->conCount);
        memset(context->conChecked, 0, sizeof(bool) * context->conCount);

        *match = context;
    }

    // Generate code for the child pattern.
    int result;
    if(pat->pats) {
        b->block = then;
        auto content = getField(b, pivot, 0, con->index + 1, con->content);
        result = resolvePat(b, &context->children, onFail, content, pat->pats);
    } else {
        result = 1;
    }

    // Handle bookkeeping for what constructors we have tested.
    if(!context->conChecked[con->index]) {
        context->conChecked[con->index] = true;
        context->checkedCount++;
    }

    // Check if the comparison result is constant.
    if(context->checkedCount >= context->conCount) {
        // The child pattern will always succeed. This means that if each other constructor
        // in this type has been checked, we will always succeed.
        if(then->instructions.size() > 0) {
            b->fun->blocks.push(then);
            jmp(initial, then);
        } else {
            b->block = initial;
        }
        return result;
    }

    // If not, generate the comparison as we would normally.
    auto end = b->block;
    b->block = initial;
    auto pivotCon = getField(b, pivot, 0, 0, &intTypes[IntType::Int]);
    auto equal = icmp(b->block, 0, pivotCon, constInt(b->block, 0, con->index, &intTypes[IntType::Int]), ICmp::eq);

    b->fun->blocks.push(then);
    je(b->block, equal, then, onFail);
    b->block = end;
    return 0;
}

int resolveArrPat(FunBuilder* b, MatchContext** match, Block* onFail, Value* pivot, ast::ArrayPat* pat) {
    jmp(b->block, onFail);
    return -1;
}

void resolveRestPat(FunBuilder* b, Matcher* m, Value* pivot, ast::RestPat* pat) {
    // Currently only used as part of an array pat.
    m->alwaysFalse = true;
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

void resolveRangePat(FunBuilder* b, Matcher* matcher, Value* pivot, ast::RangePat* pat) {
    auto from = getRangeArg(b, pat->from);
    auto to = getRangeArg(b, pat->to);

    if(!from && !to) {
        matcher->alwaysTrue = true;
    } else if(!from) {
        auto fromArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        fromArgs[0] = pivot;
        fromArgs[1] = to;
        auto fromCmp = genStaticCall(b, leHash, fromArgs, 2, 0);

        if(!fromCmp || fromCmp->type != &intTypes[IntType::Bool]) {
            if(!fromCmp || fromCmp->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            matcher->alwaysFalse = true;
        } else {
            matcher->comparison = fromCmp;
        }
    } else if(!to) {
        auto toArgs = (Value**)b->mem.alloc(sizeof(Value) * 2);
        toArgs[0] = pivot;
        toArgs[1] = from;
        auto toCmp = genStaticCall(b, geHash, toArgs, 2, 0);

        if(!toCmp || toCmp->type != &intTypes[IntType::Bool]) {
            if(!toCmp || toCmp->type->kind != Type::Error) {
                error(b, "result of a comparison must be a boolean", pat);
            }
            matcher->alwaysFalse = true;
        } else {
            matcher->comparison = toCmp;
        }
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
            matcher->alwaysFalse = true;
        } else {
            matcher->comparison = and_(b->block, 0, fromCmp, toCmp);
        }
    }
}

int resolvePat(FunBuilder* b, MatchContext** match, Block* onFail, Value* pivot, ast::Pat* pat) {
    Matcher m;

    switch(pat->kind) {
        case ast::Pat::Error:
            m.alwaysFalse = true;
            break;
        case ast::Pat::Var:
            resolveVarPat(b, &m, pivot, (ast::VarPat*)pat);
            break;
        case ast::Pat::Lit:
            resolveLitPat(b, &m, pivot, (ast::LitPat*)pat);
            break;
        case ast::Pat::Any:
            m.alwaysTrue = true;
            break;
        case ast::Pat::Tup:
            return resolveTupPat(b, match, onFail, pivot, (ast::TupPat*)pat);
        case ast::Pat::Con:
            return resolveConPat(b, match, onFail, pivot, (ast::ConPat*)pat);
        case ast::Pat::Array:
            return resolveArrPat(b, match, onFail, pivot, (ast::ArrayPat*)pat);
        case ast::Pat::Rest:
            resolveRestPat(b, &m, pivot, (ast::RestPat*)pat);
            break;
        case ast::Pat::Range:
            resolveRangePat(b, &m, pivot, (ast::RangePat*)pat);
            break;
        default:
            m.alwaysFalse = true;
    }

    if(m.alwaysTrue || (m.comparison && alwaysTrue(m.comparison))) {
        return 1;
    } else if(m.alwaysFalse || (m.comparison && alwaysFalse(m.comparison))) {
        jmp(b->block, onFail);
        return -1;
    } else {
        auto then = block(b->fun);
        then->preceding = b->block;

        je(b->block, m.comparison, then, onFail);
        b->block = then;

        return 0;
    }
}