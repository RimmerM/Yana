#pragma once

#include "../compiler/context.h"
#include "block.h"
#include "type.h"

struct Function;
struct ForeignFunction;

struct Import {
    Module* module;
    Identifier* localName;
    Array<Id> includedSymbols;
    Array<Id> excludedSymbols;
    bool qualified;
};

struct Module {
    Identifier* name;

    HashMap<Import, Id> imports;
    HashMap<Function, Id> functions;
    HashMap<ForeignFunction, Id> foreignFunctions;
    HashMap<TypeClass, Id> typeClasses;

    HashMap<Type*, Id> types;
    HashMap<Con*, Id> cons;
    HashMap<OpProperties, Id> ops;
    HashMap<Value*, Id> globals;

    Function* staticInit = nullptr;

    Arena memory;
    void* codegen = nullptr;
};

AliasType* defineAlias(Context* context, Module* in, Id name, Type* to);
RecordType* defineRecord(Context* context, Module* in, Id name, bool qualified);
Con* defineCon(Context* context, Module* in, RecordType* to, Id name, Type* content);
TypeClass* defineClass(Context* context, Module* in, Id name);
Function* defineFun(Context* context, Module* in, Id name);

Type* findType(Context* context, Module* module, Id name);
Con* findCon(Context* context, Module* module, Id name);
OpProperties* findOp(Context* context, Module* module, Id name);

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

struct ForeignFunction {
    Module* module;
    Id name;
    Id externalName;
    Id from;
    FunType* type;
};