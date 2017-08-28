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
    auto funType = (FunType*)callee->type;
    auto argCount = (U32)funType->argCount;

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    memset(args, 0, sizeof(Value*) * argCount);

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
                error(b, "function has no argument with this name", arg.value);
            }
        }

        if(found) {
            if(args[argIndex]) {
                error(b, "function argument specified more than once", arg.value);
            }

            args[argIndex] = resolveExpr(b, arg.value, 0, true);
        }

        i++;
        argList = argList->next;
    }

    // If the call used incorrect argument names this error may not trigger.
    // However, in that case we already have an error for the argument name.
    if(i != argCount) {
        error(b, "incorrect number of function arguments", nullptr);
    }

    // Check the argument types and perform implicit conversions if needed.
    for(i = 0; i < argCount; i++) {
        auto v = implicitConvert(b, args[i], funType->args[i].type, false, false);
        if(!v) {
            error(b, "incompatible type for function argument", nullptr);
        }

        args[i] = v;
    }

    return callDyn(b->block, name, callee, args, argCount);
}

static Function* resolveStaticFun(FunBuilder* b, Id funName, Value* fieldArg) {
    // TODO: Use field argument to find an instance for that type.
    auto fun = findFun(&b->context, b->fun->module, funName);
    if(!fun) {
        error(b, "no function found for this name", nullptr);
        return nullptr;
    }

    // Make sure the function definition is finished.
    resolveFun(&b->context, fun);
    return fun;
}

static Value* finishStaticCall(FunBuilder* b, Function* fun, Value** args, U32 count, Id name) {
    auto argCount = (U32)fun->args.size();

    // If the call used incorrect argument names this error may not trigger.
    // However, in that case we already have an error for the argument name.
    if(count != argCount) {
        error(b, "incorrect number of function arguments", nullptr);
    }

    // Check the argument types and perform implicit conversions if needed.
    for(U32 i = 0; i < argCount; i++) {
        auto v = implicitConvert(b, args[i], fun->args[i]->type, false, false);
        if(v) {
            args[i] = v;
        } else {
            error(b, "incompatible type for function argument", nullptr);
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

Value* genStaticCall(FunBuilder* b, Id funName, Value** args, U32 count, Id name) {
    auto fun = resolveStaticFun(b, funName, nullptr);
    if(!fun) return nullptr;

    return finishStaticCall(b, fun, args, count, name);
}

Value* resolveStaticCall(FunBuilder* b, Id funName, Value* firstArg, List<ast::TupArg>* argList, Id name) {
    auto fun = resolveStaticFun(b, funName, firstArg);
    if(!fun) return nullptr;

    auto argCount = (U32)fun->args.size();

    auto args = (Value**)b->mem.alloc(sizeof(Value*) * argCount);
    memset(args, 0, sizeof(Value*) * argCount);

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
            for(U32 a = 0; a < argCount; a++) {
                auto fa = fun->args[a];
                if(arg.name == fa->name) {
                    argIndex = fa->index;
                    found = true;
                    break;
                }
            }

            if(!found) {
                error(b, "function has no argument with this name", arg.value);
            }
        }

        if(found && argIndex < argCount) {
            if(args[argIndex]) {
                error(b, "function argument specified more than once", arg.value);
            }

            args[argIndex] = resolveExpr(b, arg.value, 0, true);
        }

        i++;
        argList = argList->next;
    }

    return finishStaticCall(b, fun, args, i, name);
}