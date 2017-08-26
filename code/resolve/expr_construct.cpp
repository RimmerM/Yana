#include <alloca.h>
#include "expr.h"
#include "../parse/ast.h"

/*
 * Handles resolving of named type construction.
 *  - Record types are constructed by the constructor name and a set of arguments for that constructor.
 *    Generic records are instantiated for the set of arguments provided.
 *  - Named primitive types (defined in builtins.cpp) are constructed like any expression,
 *    but implicitly converted to the target type with relaxed rules.
 *  - Alias types are instantiated for the set of arguments provided, if generic.
 *    The resulting type is constructed like a named primitive.
 */

static Value* explicitConstruct(FunBuilder* b, Type* type, ast::ConExpr* expr, Id name) {
    // There are a whole bunch of possible cases we could support here,
    // but most can't even be created by the parser.
    // For now we handle aliases and primitive types.
    if(type->kind == Type::Int || type->kind == Type::Float) {
        if(!expr->args || expr->args->next) {
            error(b, "incorrect number of arguments to type constructor", expr);
            return nullptr;
        }

        auto arg = resolveExpr(b, expr->args->item.value, name, true);
        return implicitConvert(b, arg, type, true, true);
    } else if(type->kind == Type::String) {
        if(!expr->args || expr->args->next) {
            error(b, "incorrect number of arguments to string constructor", expr);
            return nullptr;
        }

        auto arg = resolveExpr(b, expr->args->item.value, name, true);
        if(arg->type->kind != Type::String) {
            error(b, "strings must be constructed with a string", expr);
            return nullptr;
        }

        return arg;
    } else if(type->kind == Type::Alias) {
        // TODO: Handle generic instantiation here.
        return explicitConstruct(b, ((AliasType*)type)->to, expr, name);
    } else if(type->kind == Type::Tup) {
        // TODO: Handle tuple types.
    }

    error(b, "cannot construct this type", expr->type);
    return nullptr;
}

static Value* resolveMiscCon(FunBuilder* b, ast::ConExpr* expr, Id name) {
    auto type = findType(&b->context, b->fun->module, expr->type->con);
    if(!type) {
        error(b, "cannot find type", expr->type);
        return nullptr;
    }

    return explicitConstruct(b, type, expr, name);
}

Value* resolveCon(FunBuilder* b, ast::ConExpr* expr, Id name) {
    auto con = findCon(&b->context, b->fun->module, expr->type->con);
    if(!con) {
        return resolveMiscCon(b, expr, name);
    }

    auto content = con->content;

    // If we have more than one argument, the constructor must contain a tuple type.
    auto arg = expr->args;
    if(!arg) {
        if(content != nullptr) {
            error(b, "incorrect number of arguments to constructor", expr);
        }

        return record(b->block, name, con, nullptr);
    }

    if(!content) {
        error(b, "incorrect number of arguments to constructor", expr);
        return error(b->block, name, con->parent);
    }

    U32 argCount = 0;
    while(arg) {
        argCount++;
        arg = arg->next;
    }

    U32 contentArgs = 0;
    TupType* targetTuple = nullptr;
    if(content->kind == Type::Tup) {
        targetTuple = (TupType*)content;
        contentArgs = targetTuple->count;
    } else if(content->kind == Type::Alias) {
        auto alias = (AliasType*)content;
        if(alias->to->kind == Type::Tup) {
            targetTuple = (TupType*)alias->to;
            contentArgs = targetTuple->count;
        } else {
            contentArgs = 1;
        }
    }

    if(contentArgs != argCount) {
        error(b, "incorrect number of arguments to constructor", expr);
        return error(b->block, name, con->parent);
    }

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    memset(args, 0, sizeof(Value*) * argCount);

    arg = expr->args;
    for(U32 i = 0; i < argCount; i++) {
        auto argIndex = i;
        bool found = true;

        if(arg->item.name) {
            if(targetTuple) {
                found = false;
                for(U32 a = 0; a < contentArgs; a++) {
                    auto ta = &targetTuple->fields[a];
                    if(arg->item.name == ta->name) {
                        argIndex = ta->index;
                        found = true;
                        break;
                    }
                }
            } else {
                error(b, "constructed type has no field with this name", arg->item.value);
            }
        }

        if(found) {
            if(args[argIndex]) {
                error(b, "tuple value specified more than once", arg->item.value);
            }

            args[argIndex] = resolveExpr(b, arg->item.value, 0, true);
        } else {
            error(b, "constructed type has no field with this name", arg->item.value);
        }

        arg = arg->next;
    }

    // If the constructed type is a generic type, we instantiate it.
    if(con->parent->genCount && !con->parent->instance) {
        auto instanceArgs = (Type**)alloca(sizeof(Type*) * con->parent->genCount);
        for(U32 i = 0; i < argCount; i++) {
            // TODO: Handle higher-kinded type fields.
            auto t = con->fields[i].type;
            if(t->kind == Type::Gen) {
                auto g = (GenType*)t;
                instanceArgs[g->index] = args[i]->type;
            }
        }

        auto record = instantiateRecord(&b->context, b->fun->module, con->parent, instanceArgs, con->parent->genCount);
        con = &record->cons[con->index];
    }

    // Make sure each field gets the correct type.
    Value* targetContent;
    if(targetTuple) {
        for(U32 i = 0; i < argCount; i++) {
            args[i] = implicitConvert(b, args[i], targetTuple->fields[i].type, false, true);
        }

        targetContent = tup(b->block, 0, content, args, argCount);
    } else {
        targetContent = implicitConvert(b, args[0], con->content, false, true);
    }

    auto value = record(b->block, name, con, targetContent);
    return value;
}