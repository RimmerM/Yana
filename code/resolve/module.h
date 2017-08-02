#pragma once

#include "../compiler/context.h"
#include "block.h"
#include "type.h"

struct Function;

struct Import {
    Module* module;
    Array<Id> includedSymbols;
    Array<Id> excludedSymbols;
    Id qualifier;
    bool qualified;
};

struct Module {
    Id name;

    HashMap<Import, Id> imports;
    HashMap<Function, Id> functions;
    HashMap<TypeClass, Id> typeClasses;

    HashMap<Type*, Id> types;
    HashMap<Con*, Id> cons;
    HashMap<Value*, Id> globals;

    Function* staticInit = nullptr;

    Arena memory;
    void* codegen = nullptr;
};

AliasType* defineAlias(Module* in, Id name, Type* to);
RecordType* defineRecord(Module* in, Id name);
Con* defineCon(Module* in, RecordType* to, Id name, Type* content);
TypeClass* defineClass(Module* in, Id name);
Function* defineFun(Module* in, Id name);

struct Function {
    Module* module;
    Id name;

    Block* body = nullptr;
    Type* returnType = nullptr;
    Array<Arg> args;
    Array<Block> blocks;
    Array<InstRet*> returnPoints;

    void* codegen = nullptr;
};