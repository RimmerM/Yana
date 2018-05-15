#pragma once

#include "../compiler/context.h"
#include "block.h"
#include "type.h"

namespace ast { struct FunDecl; struct DeclExpr; struct ForeignDecl; struct Expr; struct Pat; struct Module; }

struct Module;
struct Function;
struct ForeignFunction;
struct FunBuilder;

struct Import {
    Module* module;
    Id localName;
    Array<Id> includedSymbols;
    Array<Id> excludedSymbols;
    bool qualified;
};

struct ClassFun {
    TypeClass* typeClass;
    U32 index;
    Id name;
};

struct InstanceMap {
    Array<ClassInstance*> instances; // List of implementations, sorted by descriptor.
    TypeClass* forClass;
    U32 genCount = 0; // Number of instances in each array element.
};

struct Module {
    Id id;

    HashMap<Import, Id> imports;
    HashMap<Function, Id> functions;
    HashMap<ForeignFunction, Id> foreignFunctions;
    HashMap<TypeClass, Id> typeClasses;
    HashMap<InstanceMap, Id> classInstances;
    HashMap<ClassFun, Id> classFunctions;

    HashMap<Type*, Id> types;
    HashMap<Con*, Id> cons;
    HashMap<OpProperties, Id> ops;
    HashMap<Global, Id> globals;

    HashMap<TupType*, Id> usedTuples;

    Function* staticInit = nullptr;

    Arena memory;
    void* codegen = nullptr;
};

struct ModuleHandler {
    // Returns a module for the provided identifier if it was available.
    // If not, returns null and queues the module for loading and the caller for later completion.
    // The resolver should only resolve imports and then stop if any require call returns null.
    virtual Module* require(Context* context, Module* from, Id name) = 0;
};

AliasType* defineAlias(Context* context, Module* in, Id name, Type* to);
RecordType* defineRecord(Context* context, Module* in, Id name, U32 conCount, bool qualified);
Con* defineCon(Context* context, Module* in, RecordType* to, Id name, U32 index);
TypeClass* defineClass(Context* context, Module* in, Id name);
ClassInstance* defineInstance(Context* context, Module* in, TypeClass* to, Type** args);
Function* defineFun(Context* context, Module* in, Id name);
Function* defineAnonymousFun(Context* context, Module* in);
ForeignFunction* defineForeignFun(Context* context, Module* in, Id name, FunType* type);
Global* defineGlobal(Context* context, Module* in, Id name);
Arg* defineArg(Context* context, Function* fun, Block* block, Id name, Type* type);
ClassFun* defineClassFun(Context* context, Module* module, TypeClass* typeClass, Id name, U32 index);

Type* findType(Context* context, Module* module, Id name);
Con* findCon(Context* context, Module* module, Id name);
OpProperties* findOp(Context* context, Module* module, Id name);
Global* findGlobal(Context* context, Module* module, Id name);
TypeClass* findClass(Context* context, Module* module, Id name);
ClassInstance* findInstance(Context* context, Module* module, TypeClass* typeClass, U32 index, Type** args);

struct FoundFunction {
    enum Kind {
        Static,
        Foreign,
        Class,
    };

    union {
        Function* function;
        ForeignFunction* foreignFunction;
        ClassFun classFun;
    };

    Kind kind;
    bool found;
};

FoundFunction findFun(Context* context, Module* module, Id name);

Module* resolveModule(Context* context, ModuleHandler* handler, ast::Module* ast);
void resolveFun(Context* context, Function* fun);
Value* resolveExpr(FunBuilder* b, ast::Expr* expr, Id name, bool used);

struct Function {
    Module* module;
    Id name;

    Type* returnType = nullptr;
    Array<Arg*> args;
    Array<Block*> blocks;
    Array<InstRet*> returnPoints;

    // If this function can be used as an intrinsic, this generates an inline version in the current block.
    Value* (*intrinsic)(FunBuilder* b, Value** args, U32 count, Id name) = nullptr;

    ast::FunDecl* ast = nullptr; // Set until the function is fully resolved.
    void* codegen = nullptr;

    // Each instruction in a function has a unique id.
    // This counter tracks how many we have created.
    U32 instCounter = 0;

    // Each block in a function has a unique id.
    // This counter tracks how many we have created.
    U32 blockCounter = 0;

    // Globals and functions can be interdependent.
    // This is no problem in most cases, except when their inferred types depend on each other,
    // which could cause infinite recursion.
    // We use this flag to detect that condition and throw an error.
    bool resolving = false;
};

struct ForeignFunction {
    Module* module;
    Id name;
    Id externalName;
    Id from;
    FunType* type;

    ast::ForeignDecl* ast = nullptr; // Set until the type is fully resolved.
    void* codegen = nullptr;
};

struct FunBuilder {
    FunBuilder(Function* fun, Block* block, Context& context, Arena& mem, Arena& exprMem):
        fun(fun), block(block), context(context), mem(mem), exprMem(exprMem) {}

    Function* fun;
    Block* block;
    Context& context;
    Arena& mem; // Persistent memory for this module.
    Arena& exprMem; // Temporary memory for expression resolving. Reset after each expression.
    Size funCounter = 0;
};