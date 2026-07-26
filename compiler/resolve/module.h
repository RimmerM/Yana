#pragma once

#include "block.h"

namespace ast {
struct Module;
struct Decl;
struct Expr;
}

enum class StorageClass: U8 {
    Stack,
};

// A named storage slot in a function. `convention` and `storage` are the two halves of what
// Implementation-IR.md part 2 asks a Local to carry; both stay at their defaults until the
// ownership milestone gives the resolver something to put in them.
struct Local {
    TypePtr type = nullptr;
    StringId name = 0;
    ModulePtr<Value> value = nullptr;
    ast::BindType convention = ast::BindType::Borrow;
    StorageClass storage = StorageClass::Stack;
};

struct Function {
    explicit Function(StringId name): name(name) {}

    Block* addBlock(Module& module, StringId name = 0);
    Arg* addArg(Module& module, StringId name, TypePtr type, LocationId source);
    U32 addLocal(Module& module, TypePtr type, StringId name, ModulePtr<Value> value);

    Local localAt(ModuleBase base, U32 index) { return locals.get(base, index); }
    Size localCount() { return locals.size(); }

    StringId name;
    LocationId source = kNullLocation;
    TypePtr returnType = nullptr;
    ModuleList<ModulePtr<Arg>, false> args;
    ModuleList<ModulePtr<Block>, false> blocks;
    ModuleList<Local, false> locals;
    ast::ParsePtr<ast::Decl> ast = nullptr;
    StringId exportedName = 0;
    U32 valueCounter = 0;
    bool builtin = false;
    bool used = false;
};

// One name a function is reachable under besides its own. The builtins are the only source of
// these today: they are declared under an internal name and called under an operator name, and
// several of them share one (`rem` is also `%`). This is the temporary stand-in for typeclass
// resolution described by Implementation-IR.md part 6.
struct FunctionOverload {
    StringId name = 0;
    ModulePtr<Function> function = nullptr;
    bool temporaryBuiltinResolver = false;
};

struct Module {
    Module(Context& context, StringId name, ast::ParseBase parse,
           Size typeMemory = 1024 * 1024, Size irMemory = 4 * 1024 * 1024);

    Function* addFunction(StringId name, LocationId source);
    Block* entry(Function& function);

    Context& context;
    StringId name;
    Region<GlobalRegion> types;
    Region<ModuleRegion> arena;
    ast::ParseBase parse;
    ScalarTypes scalar;
    HashMap<StringId, TypePtr> namedTypes;
    HashMap<StringId, ConstructorRef> constructors;
    HashMap<StringId, ModulePtr<Function>> functions;
    ModuleList<ModulePtr<Function>, false> functionOrder;
    ModuleList<FunctionOverload, false> overloads;
    GlobalList<GlobalPtr<TupType>> tupleTypes;
    HashMap<StringId, U8> operatorPrecedence;
};

Ptr<Module> resolveModule(Context& context, ast::Module& ast);
