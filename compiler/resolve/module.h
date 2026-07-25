#pragma once

#include "../compiler/context.h"
#include "block.h"
#include "type.h"

namespace ast { struct FunDecl; struct VarDecl; struct ForeignDecl; struct Expr; struct Pat; struct Module; }

struct Module;
struct Function;
struct ForeignFunction;
struct FunBuilder;

struct Import {
    Module* module;
    StringId localName;
    Array<StringId> includedSymbols;
    Array<StringId> excludedSymbols;
    bool qualified;
};

struct Module {
    StringId id;
    RegionBase<GlobalRegion> global;
    RegionBase<ModuleRegion> local;

    HashMap<StringId, Import> imports;
    HashMap<StringId, Function> functions;
    HashMap<StringId, ForeignFunction> foreignFunctions;

    HashMap<StringId, TypePtr> types;
    HashMap<StringId, ConPtr> cons;
    HashMap<StringId, OpProperties> ops;
    HashMap<StringId, Global> globals;

    HashMap<StringId, RegionPtr<GlobalRegion, TupType>> usedTuples;

    Function* staticInit = nullptr;

    Region<ModuleRegion> memory;
    void* codegen = nullptr;
};

struct ModuleProvider {
    // Returns a module for the provided identifier if it was available.
    // If not, returns null and queues the module for loading and the caller for later completion.
    // The resolver should only resolve imports and then stop if any require call returns null.
    virtual Module* getModule(Module* from, StringId name) = 0;
};

AliasType* defineAlias(Context* context, Module* in, StringId name, Type* to, const Location* where);
RecordType* defineRecord(Context* context, Module* in, StringId name, U32 conCount, bool qualified, const Location* where);
Con* defineCon(Context* context, Module* in, RecordType* to, StringId name, U32 index, const Location* where);
Function* defineFun(Context* context, Module* in, StringId name, const Location* where);
Function* defineAnonymousFun(Context* context, Module* in);
ForeignFunction* defineForeignFun(Context* context, Module* in, StringId name, FunType* type, const Location* where);
Global* defineGlobal(Context* context, Module* in, StringId name, const Location* where);
Arg* defineArg(Context* context, Function* fun, Block* block, StringId name, Type* type, const Location* where);

Type* findType(Context* context, Module* module, StringId name);
Con* findCon(Context* context, Module* module, StringId name);
OpProperties* findOp(Context* context, Module* module, StringId name);
Global* findGlobal(Context* context, Module* module, StringId name);

struct FoundFunction {
    enum Kind {
        Static,
        Foreign,
    };

    union {
        Function* function;
        ForeignFunction* foreignFunction;
    };

    Kind kind;
    bool found;
};

FoundFunction findFun(Context* context, Module* module, StringId name);
Function* findInstanceFun(Context* context, Module* module, Type* fieldType, StringId name);

Ptr<Module> resolveModule(Context* context, ModuleProvider* handler, ast::Module* ast);
void resolveFun(Context* context, Function* fun, bool requireBody = true);

struct TypeContext {
    Type* targetType;
    Buffer<Type*> genInstance;
};

Value* resolveExpr(FunBuilder* b, Type* targetType, ast::Expr* expr, StringId name, bool used);

StringId getDeclName(ast::VarDecl* expr);

struct Function {
    Function(ModulePtr<Module> module): module(module) {}

    ModulePtr<Module> module;
    StringId name;
    LocationId source;

    TypePtr returnType = nullptr;
    SmallList<ModuleRegion, ModulePtr<Arg>, false> args;
    SmallList<ModuleRegion, ModulePtr<Block>, false> blocks;
    SmallList<ModuleRegion, ModulePtr<InstRet>, false> returnPoints;

    RegionPtr<ast::ParseRegion, ast::FunDecl> ast = nullptr; // Set until the function is fully resolved.

    // If this function can be used as an intrinsic, this generates an inline version in the current block.
    Value* (*intrinsic)(FunBuilder* b, Value** args, U32 count, StringId name) = nullptr;
    void* codegen = nullptr;

    // Each instruction in a function has a unique id.
    // This counter tracks how many we have created.
    U32 instCounter = 0;

    // Each block in a function has a unique id.
    // This counter tracks how many we have created.
    U16 blockCounter = 0;

    // Globals and functions can be interdependent.
    // This is no problem in most cases, except when their inferred types depend on each other,
    // which could cause infinite recursion.
    // We use this flag to detect that condition and throw an error.
    bool resolving = false;
};

struct ForeignFunction {
    ModulePtr<Module> module;
    StringId name;
    StringId externalName;
    StringId from;
    GlobalPtr<FunType> type;

    RegionPtr<ast::ParseRegion, ast::ForeignDecl> ast = nullptr; // Set until the type is fully resolved.
    void* codegen = nullptr;
};

struct FunBuilder {
    FunBuilder(Function* fun, Block* block, Context& context, Region<GlobalRegion>& global, Region<ModuleRegion>& mem, Arena& exprMem):
        global(*global), module(*mem), fun(fun), block(block), context(context), mem(mem), exprMem(exprMem) {}

    GlobalBase global;
    ModuleBase module;

    Function* fun;
    Block* block;
    Context& context;
    Region<ModuleRegion>& mem; // Persistent memory for this module.
    Arena& exprMem; // Temporary memory for expression resolving. Reset after each expression.
    Size funCounter = 0;
};