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

static void matchArgs(FunBuilder* b, List<ast::TupArg>* arg, Field* fields, Args args) {
    for(U32 i = 0; i < args.count; i++) {
        Field* field = fields + i;
        bool found = true;
        auto argName = arg->item.name;

        if(argName) {
            Field* f = nullptr;
            for(U32 a = 0; a < args.count; a++) {
                auto ta = &fields[a];
                if(argName == ta->name) {
                    f = ta;
                    break;
                }
            }

            if(f) {
                field = f;
            } else {
                found = false;
            }
        }

        if(found) {
            if(args.values[field->index]) {
                error(b, "argument specified more than once"_buffer, arg->item.value);
            }

            args.values[field->index] = resolveExpr(b, field->type, arg->item.value, 0, true);
        } else {
            error(b, "constructed type has no field with this name"_buffer, arg->item.value);
        }

        arg = arg->next;
    }
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

        auto arg = resolveExpr(b, type, expr->args->item.value, name, true);
        return implicitConvert(b, arg, type, true, true);
    } else if(type->kind == Type::String) {
        if(!expr->args || expr->args->next) {
            error(b, "incorrect number of arguments to string constructor"_buffer, expr);
            return constString(b->block, name, "", 0);
        }

        auto arg = resolveExpr(b, type, expr->args->item.value, name, true);
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

        if(args.count != tup->count) {
            error(b, "invalid field count for tuple type: %@ required, but %@ were provided"_buffer, expr->type, tup->count, args.count);
        }

        matchArgs(b, expr->args, tup->fields, args);

        if(tup->named && args.count == tup->count) {
            for(U32 i = 0; i < tup->count; i++) {
                if(!args.values[i]) {
                    error(b, "no value provided for field '%@'"_buffer, expr->type, b->context.findName(tup->fields[i].name));
                    args.values[i] = error(b->block, 0, tup->fields[i].type);
                }
            }
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
    } else if(type->kind == Type::Unit) {
        if(expr->args) {
            error(b, "incorrect number of arguments to unit type constructor"_buffer, expr);
        }

        return nop(b->block, name);
    } else if(type->kind == Type::Error) {
        return error(b->block, name, type);
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

static Value* argumentCountError(FunBuilder* b, Con* con, U32 wantedCount, Node* source, Id name) {
    error(b, "incorrect number of arguments to constructor. Constructor %@ requires %@ argument(s)"_buffer, source, b->context.findName(con->name), wantedCount);
    return error(b->block, name, con->parent);
}

static Value* resolveEmptyCon(FunBuilder* b, Type* targetType, Con* con, Node* source, Id name) {
    // If we have no arguments but the record contains type arguments, we need a target type to instantiate it.
    if(con->parent->argCount > 0) {
        auto conBase = con->parent;
        if(conBase->instanceOf) conBase = conBase->instanceOf;

        RecordType* targetBase = nullptr;
        if(targetType->kind == Type::Record) {
            targetBase = (RecordType*)targetType;
            if(targetBase->instanceOf) targetBase = targetBase->instanceOf;
        }

        if(conBase == targetBase) {
            con = &((RecordType*)targetType)->cons[con->index];
        }
    }

    // Make sure the constructor is fully defined now.
    if(con->parent->argCount > 0) {
        error(b, "cannot infer type of constructor %@ in this context"_buffer, source, b->context.findName(con->name));
    }

    return record(b->block, name, con, nullptr);
}

static Value* resolveTupCon(FunBuilder* b, ast::ConExpr* expr, Con* con, TupType* content, Id name) {
    auto args = buildArgs(b, expr->args);
    if(args.count != content->count) {
        return argumentCountError(b, con, content->count, expr, name);
    }

    matchArgs(b, expr->args, content->fields, args);

    if(content->named) {
        for(U32 i = 0; i < content->count; i++) {
            if(!args.values[i]) {
                error(b, "no value provided for field '%@'"_buffer, expr->type, b->context.findName(content->fields[i].name));
                args.values[i] = error(b->block, 0, content->fields[i].type);
            }
        }
    }

    // TODO: Instantiate based on returned type.
    for(U32 i = 0; i < args.count; i++) {
        args.values[i] = implicitConvert(b, args.values[i], content->fields[i].type, true, true);
    }

    auto value = tup(b->block, 0, content, args.values, args.count);
    return record(b->block, name, con, value);
}

static Value* resolveValueCon(FunBuilder* b, ast::ConExpr* expr, Con* con, Type* content, Id name) {
    if(!expr->args || expr->args->next) {
        return argumentCountError(b, con, 1, expr, name);
    }

    auto arg = resolveExpr(b, content, expr->args->item.value, 0, true);

    // TODO: Instantiate based on returned type.
    arg = implicitConvert(b, arg, con->content, true, true);

    return record(b->block, name, con, arg);
}

Value* resolveCon(FunBuilder* b, Type* targetType, ast::ConExpr* expr, Id name) {
    auto con = findCon(&b->context, b->fun->module, expr->type->con);
    if(!con) {
        return resolveMiscCon(b, expr, name);
    }

    auto content = con->content;
    auto arg = expr->args;

    if(!content) {
        if(arg) {
            error(b, "incorrect number of arguments to constructor. Constructor %@ requires 0 arguments, but one or more was provided."_buffer, expr, b->context.findName(con->name));
        }

        return resolveEmptyCon(b, targetType, con, expr, name);
    }

    content = canonicalType(content);
    if(content->kind == Type::Tup) {
        return resolveTupCon(b, expr, con, (TupType*)content, name);
    } else {
        return resolveValueCon(b, expr, con, content, name);
    }
}