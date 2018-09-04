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

static Value* resolveTupCon(FunBuilder* b, ast::ConExpr* expr, Con* con, Type* content, Id name);
static Value* explicitConstruct(FunBuilder* b, Type* type, Type* targetType, ast::ConExpr* expr, Id name, bool required);

static Value* constructAlias(FunBuilder* b, AliasType* alias, Type* targetType, ast::ConExpr* expr, Id name) {
    auto base = alias->base();
    if(base->argCount == 0) {
        // If the alias is complete, simply forward the arguments to its canonical type.
        return explicitConstruct(b, alias->to, targetType, expr, name, true);
    } else {
        // If we have type arguments, instantiate the alias with the actual arguments, then perform any needed conversions.
        auto v = explicitConstruct(b, base->to, targetType, expr, name, false);

        auto argCount = base->argCount;
        auto instance = (Type**)alloca(sizeof(Type*) * argCount);
        set(instance, argCount, 0);

        matchGens(base->to, v->type, [&](GenType* gen, Type* target) {
            assertTrue(gen->env->container == base);
            assertTrue(gen->index < base->gen.typeCount);
            assertTrue(target->kind != Type::Gen || ((GenType*)target)->env->kind == GenEnv::Function);
            instance[gen->index] = target;
        });

        // Make sure each type argument is defined.
        // If there are remaining type arguments, we don't have enough information to determine the final type.
        auto failCount = 0u;
        for(U32 i = 0; i < argCount; i++) {
            if(instance[i] == nullptr) {
                instance[i] = &errorType;
                failCount++;
            }
        }

        if(failCount > 0) {
            error(b, "cannot infer type of alias %@ in this context"_buffer, expr, b->context.findName(alias->name));
        }

        auto targetAlias = instantiateAlias(&b->context, b->fun->module, base, instance, argCount, nullptr, true);
        return implicitConvert(b, v, targetAlias->to, true, true);
    }
}

static Value* explicitConstruct(FunBuilder* b, Type* type, Type* targetType, ast::ConExpr* expr, Id name, bool required) {
    // There are a whole bunch of possible cases we could support here,
    // but most can't even be created by the parser.
    // For now we handle aliases and primitive types.
    if(type->kind == Type::Int || type->kind == Type::Float) {
        if(!expr->args || expr->args->next) {
            error(b, "incorrect number of arguments to type constructor"_buffer, expr);
            return type->kind == Type::Int ? (Value*)constInt(b->block, name, 0, type) : constFloat(b->block, name, 0, type);
        }

        auto arg = resolveExpr(b, type, expr->args->item.value, name, true);
        auto v = implicitConvert(b, arg, type, true, required);
        return v ? v : arg;
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
        return constructAlias(b, (AliasType*)type, targetType, expr, name);
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
                auto v = implicitConvert(b, args.values[i], fieldType, true, required);
                if(v) args.values[i] = v;
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
    } else if(type->kind == Type::Record) {
        // This case can happen when explicitly constructing an alias pointing to a record.
        // We support this for single-constructor records; records with multiple constructors
        // have to be created with a constructor name rather than the alias name.
        auto record = (RecordType*)type;
        if(record->kind == RecordType::Single) {
            return resolveTupCon(b, expr, &record->cons[0], record->cons[0].content, name);
        } else {
            error(b, "record %@ can only be constructed through a constructor name"_buffer, expr, b->context.findName(record->name));
            return error(b->block, name, type);
        }
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
    } else if(!required) {
        if(!expr->args || expr->args->next) {
            error(b, "cannot convert multiple arguments to single type"_buffer, expr);
            return error(b->block, name, type);
        } else {
            return resolveExpr(b, targetType, expr->args->item.value, name, true);
        }
    }

    error(b, "cannot construct this type"_buffer, expr->type);
    return error(b->block, name, type);
}

static Value* resolveMiscCon(FunBuilder* b, Type* targetType, ast::ConExpr* expr, Id name) {
    auto type = findType(&b->context, b->fun->module, expr->type->con);
    if(!type) {
        error(b, "cannot find type"_buffer, expr->type);
        return nullptr;
    }

    return explicitConstruct(b, type, targetType, expr, name, true);
}

static Value* argumentCountError(FunBuilder* b, Con* con, U32 wantedCount, Node* source, Id name) {
    error(b, "incorrect number of arguments to constructor. Constructor %@ requires %@ argument(s)"_buffer, source, b->context.findName(con->name), wantedCount);
    return error(b->block, name, con->parent);
}

static Con* targetCon(Con* con, Type* targetType) {
    if(con->parent->argCount > 0) {
        auto conBase = con->parent->base();

        RecordType* targetBase = nullptr;
        if(targetType) {
            targetType = canonicalType(targetType);
            if(targetType->kind == Type::Record) {
                targetBase = ((RecordType *) targetType)->base();
            }
        }

        if(conBase == targetBase) {
            con = &((RecordType*)targetType)->cons[con->index];
        }
    }

    return con;
}

static Value* resolveEmptyCon(FunBuilder* b, Con* con, Node* source, Id name) {
    // Make sure the constructor is fully defined.
    // If it still contains type arguments, we don't have enough information to determine the final type.
    if(con->parent->argCount > 0) {
        error(b, "cannot infer type of constructor %@ in this context"_buffer, source, b->context.findName(con->name));
    }

    return record(b->block, name, con, nullptr);
}

static Value* resolveTupCon(FunBuilder* b, ast::ConExpr* expr, Con* con, Type* content, Id name) {
    auto args = buildArgs(b, expr->args);
    Field* fields;
    U32 fieldCount;
    Field dummyField;

    if(content->kind == Type::Tup) {
        // TODO: Support creation from an existing tuple.
        auto tup = (TupType*)content;
        if(args.count != tup->count) {
            return argumentCountError(b, con, tup->count, expr, name);
        }

        // Match the provided arguments to fields in the target type.
        matchArgs(b, expr->args, tup->fields, args);

        // If the target type uses named fields, make sure that each field was actually provided.
        // Checking the argument count is not enough, since there may be arguments with unknown or duplicate field names.
        if(tup->named) {
            for(U32 i = 0; i < tup->count; i++) {
                if(!args.values[i]) {
                    error(b, "no value provided for field '%@'"_buffer, expr->type, b->context.findName(tup->fields[i].name));
                    args.values[i] = error(b->block, 0, tup->fields[i].type);
                }
            }
        }

        fields = tup->fields;
        fieldCount = tup->count;
    } else {
        if(args.count != 1) {
            return argumentCountError(b, con, 1, expr, name);
        }

        args.values[0] = resolveExpr(b, content, expr->args->item.value, 0, true);

        fields = &dummyField;
        fieldCount = 1;
        dummyField = { content, nullptr, 0, 0 };
    }

    auto base = con->parent->base();
    if(base->argCount) {
        // If the created type contains type arguments, match any generic types to the actual argument types.
        // This only solves type arguments that are used in this constructor -
        // any additional ones will have to have been provided by the target type.
        auto argCount = base->argCount;
        auto instance = (Type**)alloca(sizeof(Type*) * argCount);
        set(instance, argCount, 0);

        // When we resolve the construction of a generic record inside of another generic record,
        // we get the situation where the nested record is already instantiated and thus has no arguments.
        // However, to resolve everything correctly, we still need to instantiate the type with the provided values.
        if(con->parent->instanceOf) {
            copy(con->parent->instance, instance, argCount);
        }

        auto baseCon = base->cons[con->index].content;
        if(baseCon->kind == Type::Tup) {
            fields = ((TupType*)baseCon)->fields;
        } else {
            dummyField = {baseCon, nullptr, 0, 0};
        }

        auto changeCount = 0u;
        for(U32 i = 0; i < fieldCount; i++) {
            matchGens(fields[i].type, args.values[i]->type, [&](GenType* gen, Type* target) {
                assertTrue(gen->env->container == base);
                assertTrue(gen->index < argCount);
                assertTrue(target->kind != Type::Gen || ((GenType*)target)->env->kind == GenEnv::Function);
                instance[gen->index] = target;
                changeCount++;
            });
        }

        // Make sure each type argument is defined.
        // If there are remaining type arguments, we don't have enough information to determine the final type.
        auto failCount = 0u;
        for(U32 i = 0; i < argCount; i++) {
            if(instance[i] == nullptr) {
                instance[i] = &errorType;
                failCount++;
            }
        }

        if(failCount > 0) {
            error(b, "cannot infer type of constructor %@ in this context"_buffer, expr, b->context.findName(con->name));
        }

        if(changeCount > 0) {
            auto targetType = instantiateRecord(&b->context, b->fun->module, base, instance, argCount, nullptr, true);
            con = &targetType->cons[con->index];
            content = canonicalType(con->content);

            if(content->kind == Type::Tup) {
                fields = ((TupType*)content)->fields;
            } else {
                dummyField = {content, nullptr, 0, 0};
            }
        }
    }

    // Finally, make sure the final field types are correct and perform implicit conversions where possible.
    for(U32 i = 0; i < args.count; i++) {
        args.values[i] = implicitConvert(b, args.values[i], fields[i].type, true, true);
    }

    Value* value;
    if(content->kind == Type::Tup) {
        value = tup(b->block, 0, content, args.values, args.count);
    } else {
        value = args.values[0];
    }

    return record(b->block, name, con, value);
}

Value* resolveCon(FunBuilder* b, Type* targetType, ast::ConExpr* expr, Id name) {
    // Check if there is a record constructor for this name.
    // If not, try to explicitly construct the corresponding type instead.
    auto con = findCon(&b->context, b->fun->module, expr->type->con);
    if(!con) {
        return resolveMiscCon(b, targetType, expr, name);
    }

    // If we have a target type, match it to the constructor to get additional type constraints if possible.
    con = targetCon(con, targetType);

    auto content = con->content;
    auto arg = expr->args;

    if(!content) {
        if(arg) {
            error(b, "incorrect number of arguments to constructor. Constructor %@ requires 0 arguments, but one or more was provided."_buffer, expr, b->context.findName(con->name));
        }

        return resolveEmptyCon(b, con, expr, name);
    }

    content = canonicalType(content);
    return resolveTupCon(b, expr, con, content, name);
}