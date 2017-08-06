#pragma once

#include "../compiler/context.h"
#include "block.h"
#include "type.h"

namespace ast { struct FunDecl; struct DeclExpr; struct ForeignDecl; struct Expr; struct Module; }

struct Module;
struct Function;
struct ForeignFunction;
struct FunBuilder;

struct Import {
    Module* module;
    Identifier* localName;
    Array<Id> includedSymbols;
    Array<Id> excludedSymbols;
    bool qualified;
};

struct Global {
    Module* module;
    Type* type = nullptr;
    Id name;

    ast::DeclExpr* ast = nullptr; // Set until the function is fully resolved.

    // Globals and functions can be interdependent.
    // This is no problem in most cases, except when their inferred types depend on each other,
    // which could cause infinite recursion.
    // We use this flag to detect that condition and throw an error.
    bool resolving;
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
    HashMap<Global, Id> globals;

    Function* staticInit = nullptr;

    Arena memory;
    void* codegen = nullptr;
};

struct ModuleHandler {
    // Returns a module for the provided identifier if it was available.
    // If not, returns null and queues the module for loading and the caller for later completion.
    // The resolver should only resolve imports and then stop if any require call returns null.
    virtual Module* require(Context* context, Module* from, Identifier* id) = 0;
};

AliasType* defineAlias(Context* context, Module* in, Id name, Type* to);
RecordType* defineRecord(Context* context, Module* in, Id name, bool qualified);
Con* defineCon(Context* context, Module* in, RecordType* to, Id name, Type* content);
TypeClass* defineClass(Context* context, Module* in, Id name);
Function* defineFun(Context* context, Module* in, Id name);
ForeignFunction* defineForeignFun(Context* context, Module* in, Id name, FunType* type);
Global* defineGlobal(Context* context, Module* in, Id name);
Arg* defineArg(Context* context, Function* fun, Id name, Type* type);

Type* findType(Context* context, Module* module, Id name);
Con* findCon(Context* context, Module* module, Id name);
OpProperties* findOp(Context* context, Module* module, Id name);

Module* resolveModule(Context* context, ModuleHandler* handler, ast::Module* ast);
void resolveFun(Context* context, Function* fun);
Value* resolveExpr(FunBuilder* b, ast::Expr* expr, Id name, bool used);

struct Function {
    Module* module;
    Id name;

    Block* body = nullptr;
    Type* returnType = nullptr;
    Array<Arg> args;
    Array<Block> blocks;
    Array<InstRet*> returnPoints;

    ast::FunDecl* ast = nullptr; // Set until the function is fully resolved.
    void* codegen = nullptr;

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
    FunBuilder(Function* fun, Block* block, Context& context, Arena& mem): fun(fun), block(block), context(context), mem(mem) {}

    Function* fun;
    Block* block;
    Context& context;
    Arena& mem;
    Size funCounter = 0;
};