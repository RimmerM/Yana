#pragma once

#include "../compiler/context.h"
#include "block.h"

struct Function;

struct Module {
    Id name;
    HashMap<Function*, Id> functions;
    HashMap<Type*, Id> types;
    HashMap<Value*, Id> globals;

    Function* staticInit = nullptr;

    Arena memory;
    void* codegen = nullptr;
};

struct Function {
    Module* module;
    Id name;
    Block* body;
    Array<InstRet*> returnPoints;
    Type* returnType;
    Array<Arg*> args;

    void* codegen = nullptr;
};
