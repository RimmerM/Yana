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

struct Args {
    Value** values;
    U32 count;
};

static Args buildArgs(FunBuilder* b, List<ast::TupArg>* arg) {
    U32 argCount = 0;
    while(arg) {
        argCount++;
        arg = arg->next;
    }

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    set(args, argCount, 0);

    return {args, argCount};
}

template<class F>
static void matchArgs(FunBuilder* b, List<ast::TupArg>* arg, Args args, F f) {
    for(U32 i = 0; i < args.count; i++) {
        auto argIndex = i;
        bool found = true;

        if(arg->item.name) {
            auto foundIndex = f(i, arg->item.name);
            if(foundIndex >= 0) {
                argIndex = foundIndex;
            } else {
                found = false;
            }
        }

        if(found) {
            if(args.values[argIndex]) {
                error(b, "argument specified more than once"_buffer, arg->item.value);
            }

            args.values[argIndex] = resolveExpr(b, arg->item.value, 0, true);
        } else {
            error(b, "constructed type has no field with this name"_buffer, arg->item.value);
        }

        arg = arg->next;
    }
}

static auto tupleArgFinder(Field* fields, U32 count) {
    return [=](U32 i, Id fieldName) -> int {
        if(!fieldName) return i;

        int index = -1;
        for(U32 a = 0; a < count; a++) {
            auto ta = &fields[a];
            if(fieldName == ta->name) {
                index = ta->index;
                break;
            }
        }

        return index;
    };
}

static Value* explicitConstruct(FunBuilder* b, Type* type, ast::ConExpr* expr, Id name) {
    // There are a whole bunch of possible cases we could support here,
    // but most can't even be created by the parser.
    // For now we handle aliases and primitive types.
    if(type->kind == Type::Int || type->kind == Type::Float) {
        if(!expr->args || expr->args->next) {
            error(b, "incorrect number of arguments to type constructor"_buffer, expr);
            return type->kind == Type::Int ? (Value*)constInt(b->block, name, 0, type) : constFloat(b->block, name, 0, type);
        }

        auto arg = resolveExpr(b, expr->args->item.value, name, true);
        return implicitConvert(b, arg, type, true, true);
    } else if(type->kind == Type::String) {
        if(!expr->args || expr->args->next) {
            error(b, "incorrect number of arguments to string constructor"_buffer, expr);
            return constString(b->block, name, "", 0);
        }

        auto arg = resolveExpr(b, expr->args->item.value, name, true);
        if(arg->type->kind != Type::String) {
            error(b, "strings must be constructed with a string"_buffer, expr);
            return constString(b->block, name, "", 0);
        }

        return arg;
    } else if(type->kind == Type::Alias) {
        // This won't handle aliases to record types.
        // Constructing a record using the alias name is impossible,
        // since record construction requires a constructor name instead of a type name.
        // TODO: Handle generic instantiation here.
        return explicitConstruct(b, ((AliasType*)type)->to, expr, name);
    } else if(type->kind == Type::Tup) {
        auto tup = (TupType*)type;
        auto args = buildArgs(b, expr->args);
        matchArgs(b, expr->args, args, tupleArgFinder(tup->fields, tup->count));

        if(args.count < tup->count) {
            error(b, "missing fields for tuple type"_buffer, expr->type);
        } else if(args.count > tup->count) {
            error(b, "too many fields for tuple type"_buffer, expr->type);
        }

        // Implicitly convert each field to the correct type.
        // Generic fields are automatically instantiated to the actual type from the arguments.
        // If any generic fields were instantiated, we have to generate a new TupType for this expression.
        U32 genCount = 0;
        for(U32 i = 0; i < args.count; i++) {
            auto fieldType = tup->fields[i].type;
            if(fieldType->kind == Type::Gen) {
                genCount++;
            } else {
                args.values[i] = implicitConvert(b, args.values[i], fieldType, true, true);
            }
        }

        auto finalType = tup;
        if(genCount > 0) {
            auto finalFields = (Field*)alloca(sizeof(Field) * args.count);
            for(U32 i = 0; i < args.count; i++) {
                finalFields[i].type = args.values[i]->type;
                finalFields[i].name = tup->fields[i].name;
            }

            finalType = resolveTupType(&b->context, b->fun->module, finalFields, args.count);
        }

        return ::tup(b->block, name, finalType, args.values, args.count);
    } else if(type->kind == Type::Array) {
        // TODO
        error(b, "not implemented"_buffer, expr);
        return error(b->block, name, type);
    } else if(type->kind == Type::Map) {
        // TODO
        error(b, "not implemented"_buffer, expr);
        return error(b->block, name, type);
    } else if(type->kind == Type::Fun) {
        // TODO
        error(b, "not implemented"_buffer, expr);
        return error(b->block, name, type);
    } else if(type->kind == Type::Unit || type->kind == Type::Error) {
        return nullptr;
    }

    error(b, "cannot construct this type"_buffer, expr->type);
    return error(b->block, name, type);
}

static Value* resolveMiscCon(FunBuilder* b, ast::ConExpr* expr, Id name) {
    auto type = findType(&b->context, b->fun->module, expr->type->con);
    if(!type) {
        error(b, "cannot find type"_buffer, expr->type);
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
            error(b, "incorrect number of arguments to constructor"_buffer, expr);
        }

        return record(b->block, name, con, nullptr);
    }

    if(!content) {
        error(b, "incorrect number of arguments to constructor"_buffer, expr);
        return error(b->block, name, con->parent);
    }

    auto args = buildArgs(b, expr->args);

    U32 contentArgs = 1;
    TupType* targetTuple = nullptr;
    if(content->kind == Type::Tup) {
        targetTuple = (TupType*)content;
        contentArgs = targetTuple->count;
    } else if(content->kind == Type::Alias) {
        auto alias = (AliasType*)content;
        if(alias->to->kind == Type::Tup) {
            targetTuple = (TupType*)alias->to;
            contentArgs = targetTuple->count;
        }
    }

    if(contentArgs != args.count) {
        error(b, "incorrect number of arguments to constructor"_buffer, expr);
        return error(b->block, name, con->parent);
    }

    if(targetTuple) {
        matchArgs(b, expr->args, args, tupleArgFinder(targetTuple->fields, targetTuple->count));
    } else {
        matchArgs(b, expr->args, args, [=](U32 i, Id fieldName) -> int {
            if(fieldName) return -1;
            return i;
        });
    }

    // If the constructed type is a generic type, we instantiate it.
    if(con->parent->argCount) {
        auto instanceArgs = (Type**)alloca(sizeof(Type*) * con->parent->argCount);

        // TODO: Handle GenTypes not used in the constructor.
        if(targetTuple) {
            for(U32 i = 0; i < args.count; i++) {
                // TODO: Handle higher-kinded type fields.
                auto t = targetTuple->fields[i].type;
                if(t->kind == Type::Gen) {
                    auto g = (GenType*)t;
                    instanceArgs[g->index] = args.values[i]->type;
                }
            }
        } else if(content->kind == Type::Gen) {
            auto g = (GenType*)content;
            instanceArgs[g->index] = args.values[0]->type;
        }

        auto record = instantiateRecord(&b->context, b->fun->module, con->parent, instanceArgs, con->parent->argCount, true);
        con = &record->cons[con->index];
    }

    // Make sure each field gets the correct type.
    Value* targetContent;
    if(targetTuple) {
        for(U32 i = 0; i < args.count; i++) {
            args.values[i] = implicitConvert(b, args.values[i], targetTuple->fields[i].type, true, true);
        }

        targetContent = tup(b->block, 0, content, args.values, args.count);
    } else {
        targetContent = implicitConvert(b, args.values[0], con->content, true, true);
    }

    auto value = record(b->block, name, con, targetContent);
    return value;
}