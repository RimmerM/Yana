#pragma once

#include "inst.h"

struct Module;
struct Function;

/*
 * Typeclasses.
 *
 * A class is a set of function signatures written against the type variables of its own generic
 * context; an instance supplies one implementation per signature for one concrete assignment of
 * those variables. Calling a class function is therefore two steps - work out what the class
 * variables must be at this call site, then find the instance for them - and both steps are
 * driven by matchType() against the signature, which is why a variable that only appears in the
 * return type is inferable exactly like one that appears in an argument.
 */

struct ClassFun {
    StringId name = 0;

    // The signature, as a body-less Function: its args carry the declared types and names, and
    // its returnType the declared result. Types in it belong to the class's generic context.
    ModulePtr<Function> fun = nullptr;
    U16 index = 0;

    // Recorded at declaration time so that two functions of one class can share a name when
    // their arities differ - a class declares both the binary and the unary `-`.
    U16 arity = 0;
};

struct TypeClass {
    TypeClass(StringId name, GlobalPtr<GenEnv> gen): name(name), gen(gen) {}

    StringId name;
    GlobalPtr<GenEnv> gen;
    GlobalList<ClassFun> functions;
    Module* module = nullptr;
    ast::ParsePtr<ast::Decl> ast = nullptr;
    LocationId source = kNullLocation;
    bool ready = false;
};

struct ClassInstance {
    explicit ClassInstance(GlobalPtr<TypeClass> typeClass): typeClass(typeClass) {}

    GlobalPtr<TypeClass> typeClass;

    // One concrete type per class type variable, and one implementation per class function.
    ModuleList<TypePtr, false> forTypes;
    ModuleList<ModulePtr<Function>, false> functions;

    Module* module = nullptr;
    LocationId source = kNullLocation;
};

// Where a class function was found: the class, and which of its signatures.
struct ClassFunRef {
    GlobalPtr<TypeClass> typeClass = nullptr;
    StringId name = 0;
    U16 index = 0;
};
