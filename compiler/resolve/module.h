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

struct InstanceMap {
    Array<ClassInstance*> instances; // List of implementations, sorted by descriptor.
    TypeClass* forClass;
    U32 genCount = 0; // Number of instances in each array element.
};

struct InstanceList {
    Type* type;
    Array<Function> functions;
};

struct Module {
    StringId id;

    HashMap<StringId, Import> imports;
    HashMap<StringId, Function> functions;
    HashMap<StringId, InstanceList*> typeInstances;
    HashMap<StringId, InstanceList*> namedTypeInstances;
    HashMap<StringId, ForeignFunction> foreignFunctions;
    HashMap<StringId, TypeClass> typeClasses;
    HashMap<StringId, InstanceMap> classInstances;
    HashMap<StringId, ClassFun*> classFunctions;

    HashMap<StringId, Type*> types;
    HashMap<StringId, Con*> cons;
    HashMap<StringId, OpProperties> ops;
    HashMap<StringId, Global> globals;

    HashMap<StringId, TupType*> usedTuples;

    Function* staticInit = nullptr;

    Arena memory;
    void* codegen = nullptr;
};

struct ModuleProvider {
    // Returns a module for the provided identifier if it was available.
    // If not, returns null and queues the module for loading and the caller for later completion.
    // The resolver should only resolve imports and then stop if any require call returns null.
    virtual Module* getModule(Module* from, StringId name) = 0;
};

AliasType* defineAlias(Context* context, Module* in, StringId name, Type* to, const Node* where);
RecordType* defineRecord(Context* context, Module* in, StringId name, U32 conCount, bool qualified, const Node* where);
Con* defineCon(Context* context, Module* in, RecordType* to, StringId name, U32 index, const Node* where);
TypeClass* defineClass(Context* context, Module* in, StringId name, U32 funCount, const Node* where);
ClassInstance* defineInstance(Context* context, Module* in, TypeClass* to, Type** args, const Node* where);
Function* defineFun(Context* context, Module* in, StringId name, const Node* where);
Function* defineAnonymousFun(Context* context, Module* in);
ForeignFunction* defineForeignFun(Context* context, Module* in, StringId name, FunType* type, const Node* where);
Global* defineGlobal(Context* context, Module* in, StringId name, const Node* where);
Arg* defineArg(Context* context, Function* fun, Block* block, StringId name, Type* type, const Node* where);
ClassFun* defineClassFun(Context* context, Module* module, TypeClass* typeClass, StringId name, U32 index, const Node* where);

Type* findType(Context* context, Module* module, StringId name);
Con* findCon(Context* context, Module* module, StringId name);
OpProperties* findOp(Context* context, Module* module, StringId name);
Global* findGlobal(Context* context, Module* module, StringId name);
TypeClass* findClass(Context* context, Module* module, StringId name);
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
        ClassFun* classFun;
    };

    Kind kind;
    bool found;
};

FoundFunction findFun(Context* context, Module* module, StringId name);
Function* findInstanceFun(Context* context, Module* module, Type* fieldType, StringId name);

Module* resolveModule(Context* context, ModuleProvider* handler, ast::Module* ast);
void resolveFun(Context* context, Function* fun, bool requireBody = true);

struct TypeContext {
    Type* targetType;
    Buffer<Type*> genInstance;
};

Value* resolveExpr(FunBuilder* b, Type* targetType, ast::Expr* expr, StringId name, bool used);

StringId getDeclName(ast::VarDecl* expr);

struct Function {
    Function(): gen(this, GenEnv::Function) {}

    Module* module;
    StringId name;
    Node source;

    Type* returnType = nullptr;
    Array<Arg*> args;
    Array<Block*> blocks;
    Array<InstRet*> returnPoints;

    GenEnv gen;
    Function* instanceOf = nullptr;
    Type** instance = nullptr; // if instanceOf is set, this contains a list Type*[instanceOf->genCount].

    // If this function can be used as an intrinsic, this generates an inline version in the current block.
    Value* (*intrinsic)(FunBuilder* b, Value** args, U32 count, StringId name) = nullptr;

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
    StringId name;
    StringId externalName;
    StringId from;
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