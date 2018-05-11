#include <alloca.h>
#include "expr.h"
#include "../parse/ast.h"

/*
 * Handles resolving of function calls.
 * Calls can be either static (the called function is a known Function)
 * or dynamic (the function is a Value of function type).
 * Both call types can also be either normal or generic. Generic calls need special handling to forward
 * information about the unknown types used in them. The information required is very platform-specific,
 * so we represent it in an abstract way in the IR.
 */

Value* resolveDynCall(FunBuilder* b, Value* callee, List<ast::TupArg>* argList, Id name) {
    auto funType = (FunType*)canonicalType(callee->type);
    auto argCount = (U32)funType->argCount;

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    set(args, argCount, 0);

    U32 i = 0;
    while(argList) {
        auto arg = argList->item;
        auto argIndex = i;
        bool found = true;

        if(arg.name) {
            found = false;
            for(U32 a = 0; a < argCount; a++) {
                auto fa = funType->args[a];
                if(arg.name == fa.name) {
                    argIndex = fa.index;
                    found = true;
                }
            }

            if(!found) {
                error(b, "function has no argument with this name"_buffer, arg.value);
            }
        }

        if(found) {
            if(args[argIndex]) {
                error(b, "function argument specified more than once"_buffer, arg.value);
            }

            args[argIndex] = resolveExpr(b, arg.value, 0, true);
        }

        i++;
        argList = argList->next;
    }

    // If the call used incorrect argument names this error may not trigger.
    // However, in that case we already have an error for the argument name.
    if(i != argCount) {
        error(b, "incorrect number of function arguments"_buffer, nullptr);
    }

    // Check the argument types and perform implicit conversions if needed.
    for(i = 0; i < argCount; i++) {
        auto v = implicitConvert(b, args[i], funType->args[i].type, false, false);
        if(!v) {
            error(b, "incompatible type for function argument"_buffer, nullptr);
        }

        args[i] = v;
    }

    return callDyn(b->block, name, callee, funType->result, args, argCount);
}

static FoundFunction resolveStaticFun(FunBuilder* b, Id funName, Value* fieldArg) {
    // TODO: Use field argument to find an instance for that type.
    auto fun = findFun(&b->context, b->fun->module, funName);
    if(!fun.found) {
        error(b, "no function found for this name"_buffer, nullptr);
        return fun;
    }

    // Make sure the function definition is finished.
    if(fun.kind == FoundFunction::Static) {
        resolveFun(&b->context, fun.function);
    }

    return fun;
}

static Value* finishStaticCall(FunBuilder* b, Function* fun, Value** args, U32 count, Id name) {
    auto argCount = (U32)fun->args.size();

    // If the call used incorrect argument names this error may not trigger.
    // However, in that case we already have an error for the argument name.
    if(count != argCount) {
        error(b, "incorrect number of function arguments"_buffer, nullptr);
    }

    // Check the argument types and perform implicit conversions if needed.
    for(U32 i = 0; i < argCount; i++) {
        auto v = implicitConvert(b, args[i], fun->args[i]->type, false, false);
        if(v) {
            args[i] = v;
        } else {
            error(b, "incompatible type for function argument"_buffer, nullptr);
            args[i] = error(b->block, 0, fun->args[i]->type);
        }
    }

    // If the function is an intrinsic, we use that instead.
    if(fun->intrinsic) {
        return fun->intrinsic(b, args, argCount, name);
    } else {
        return call(b->block, name, fun, args, argCount);
    }
}

static Value* finishForeignCall(FunBuilder* b, ForeignFunction* fun, Value** args, U32 count, Id name) {
    // TODO
    return nullptr;
}

static Value* finishClassCall(FunBuilder* b, ClassFun fun, Value** args, U32 count, Id name) {
    auto funType = &fun.typeClass->functions[fun.index];

    // If the call used incorrect argument names this error may not trigger.
    // However, in that case we already have an error for the argument name.
    if(count != funType->argCount) {
        error(b, "incorrect number of function arguments"_buffer, nullptr);
    }

    // Class functions must use each type argument in their signatures.
    // This ensures that we always can infer what instance to use.
    // TODO: Handle functions where the instance type depends solely on the return type.
    auto classArgs = (Type**)alloca(sizeof(Type*) * fun.typeClass->argCount);
    set(classArgs, fun.typeClass->argCount, 0);

    for(U32 i = 0; i < count; i++) {
        auto a = funType->args[i].type;
        if(a->kind == Type::Gen) {
            classArgs[((GenType*)a)->index] = args[i]->type;
        }
    }

    auto instance = findInstance(&b->context, b->fun->module, fun.typeClass, fun.index, classArgs);
    if(!instance) {
        error(b, "cannot find an implementation of class for these arguments"_buffer, nullptr);
        return error(b->block, name, &errorType);
    }

    auto f = instance->instances[fun.index];
    return finishStaticCall(b, f, args, count, name);
}

Value* genStaticCall(FunBuilder* b, Id funName, Value** args, U32 count, Id name) {
    auto fun = resolveStaticFun(b, funName, nullptr);
    if(!fun.found) return nullptr;

    switch(fun.kind) {
        case FoundFunction::Static:
            return finishStaticCall(b, fun.function, args, count, name);
        case FoundFunction::Foreign:
            return finishForeignCall(b, fun.foreignFunction, args, count, name);
        case FoundFunction::Class:
            return finishClassCall(b, fun.classFun, args, count, name);
    }
}

Value* resolveStaticCall(FunBuilder* b, Id funName, Value* firstArg, List<ast::TupArg>* argList, Id name) {
    auto fun = resolveStaticFun(b, funName, firstArg);
    if(!fun.found) return nullptr;

    U32 argCount;
    Arg** sourceArgs = nullptr;
    FunArg* sourceFunArgs = nullptr;

    switch(fun.kind) {
        case FoundFunction::Static:
            argCount = (U32)fun.function->args.size();
            sourceArgs = fun.function->args.pointer();
            break;
        case FoundFunction::Foreign:
            argCount = fun.foreignFunction->type->argCount;
            sourceFunArgs = fun.foreignFunction->type->args;
            break;
        case FoundFunction::Class:
            argCount = fun.classFun.typeClass->functions[fun.classFun.index].argCount;
            sourceFunArgs = fun.classFun.typeClass->functions[fun.classFun.index].args;
            break;
    }

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    set(args, argCount, 0);

    U32 i = 0;
    if(firstArg) {
        args[0] = firstArg;
        i++;
    }

    while(argList) {
        auto arg = argList->item;
        auto argIndex = i;
        bool found = true;

        if(arg.name) {
            found = false;
            if(sourceArgs) {
                for(U32 a = 0; a < argCount; a++) {
                    auto fa = sourceArgs[a];
                    if(arg.name == fa->name) {
                        argIndex = fa->index;
                        found = true;
                        break;
                    }
                }
            } else if(sourceFunArgs) {
                for(U32 a = 0; a < argCount; a++) {
                    auto fa = &sourceFunArgs[a];
                    if(arg.name == fa->name) {
                        argIndex = fa->index;
                        found = true;
                        break;
                    }
                }
            }

            if(!found) {
                error(b, "function has no argument with this name"_buffer, arg.value);
            }
        }

        if(found && argIndex < argCount) {
            if(args[argIndex]) {
                error(b, "function argument specified more than once"_buffer, arg.value);
            }

            args[argIndex] = resolveExpr(b, arg.value, 0, true);
        }

        i++;
        argList = argList->next;
    }

    switch(fun.kind) {
        case FoundFunction::Static:
            return finishStaticCall(b, fun.function, args, i, name);
        case FoundFunction::Foreign:
            return finishForeignCall(b, fun.foreignFunction, args, i, name);
        case FoundFunction::Class:
            return finishClassCall(b, fun.classFun, args, i, name);
    }
}