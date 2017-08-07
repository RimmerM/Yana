#pragma once

#include "../compiler/context.h"

namespace ast { struct AliasDecl; struct DataDecl; struct ClassDecl; struct Type; }

struct Module;
struct Function;

struct Type {
    enum Kind {
        Error,
        Unit,
        Gen,
        Int,
        Float,
        String,
        Ref,
        Ptr,
        Fun,
        Array,
        Map,
        Tup,
        Record,
        Alias,
    };

    Kind kind;

    // We store a reference to some of the of types that reference a single type.
    // This is an efficient way to make sure that only a single instance is created.
    Type* refTo = nullptr;
    Type* ptrTo = nullptr;
    Type* arrayTo = nullptr;

    Type(Kind kind): kind(kind) {}
};

struct TypeClass {
    ast::ClassDecl* ast; // Set until the type is fully resolved.

    Id name;
    Array<Type*> parameters;
    Array<struct FunType*> functions;
};

struct ClassInstance {
    Module* module;
    TypeClass* typeClass;
    Type* forType;
    HashMap<FunType*, Function*, nullptr> implementations;
};

struct GenField {
    Type* type;
    struct GenType* gen;
    Id name;
    bool mut;
};

struct GenType: Type {
    GenType(U32 index): Type(Gen), index(index) {}

    U32 index;
    ::Array<GenField> fields;
    ::Array<TypeClass*> classes;
};

struct IntType: Type {
    enum Width: U8 {
        Bool,
        Int,
        Long,
        KindCount,
    };

    IntType(U16 bits, Width width): Type(Kind::Int), bits(bits), width(width) {}

    U16 bits;
    Width width;
};

struct FloatType: Type {
    enum Width: U8 {
        F16,
        F32,
        F64,
        KindCount
    };

    FloatType(U16 bits, Width width): Type{Float}, bits(bits), width(width) {}

    U16 bits;
    Width width;
};

// A reference to a GC'd value on the heap.
struct RefType: Type {
    RefType(Type* to): Type(Ref), to(to) {}
    Type* to;
};

// An untraced pointer to any value.
struct PtrType: Type {
    PtrType(Type* to): Type(Ptr), to(to) {}
    Type* to;
};

struct FunArg {
    Type* type;
    Id name;
    U32 index;
};

struct FunType: Type {
    FunType(): Type(Fun) {}

    FunArg* args;
    Type* result;
    Size argCount;
};

struct ArrayType: Type {
    ArrayType(Type* content): Type(Array), content(content) {}
    Type* content;
};

struct MapType: Type {
    MapType(Type* from, Type* to): Type(Map), from(from), to(to) {}
    Type* from, *to;
};

struct Field {
    Type* type;
    Type* container;
    Id name;
    U32 index;
};

struct TupLookup {
    Type** layout = nullptr;
    HashMap<TupLookup, Size> next;
    U32 depth = 0;
};

struct TupType: Type {
    TupType(): Type(Tup) {}
    Field* fields;
    Type** layout;
    U32 count;
};

struct Con {
    Id name;
    U32 index;
    struct RecordType* parent;
    Type* content;
};

struct RecordType: Type {
    enum Kind {
        Enum,
        Single,
        Multi,
    };

    RecordType(): Type(Record) {}

    ast::DataDecl* ast; // Set until the type is fully resolved.
    ::Array<Con> cons;
    ::Array<Type*> gens;
    Id name;
    Kind kind = Multi;
    bool qualified; // Set if the type constructors are namespaced within the type.
};

struct AliasType: Type {
    AliasType(): Type(Alias) {}

    ast::AliasDecl* ast; // Set until the type is fully resolved.
    ::Array<Type*> gens;
    Type* to;
    Id name;
};

// Global instances of the basic builtin types.
extern Type unitType;
extern FloatType floatTypes[FloatType::KindCount];
extern IntType intTypes[IntType::KindCount];
extern Type stringType;

// Returns a pointer to the provided type.
Type* getPtr(Module* module, Type* to);

// Returns a reference to the provided type.
Type* getRef(Module* module, Type* to);

// Returns an array type of the provided type.
Type* getArray(Module* module, Type* to);

// Finishes the definition of a type defined in the module, if needed.
Type* resolveDefinition(Context* context, Module* module, Type* type);

// Finds the matching type for the provided ast.
Type* resolveType(Context* context, Module* module, ast::Type* type);

// Checks if the two provided types are the same.
bool compareTypes(Context* context, Type* lhs, Type* rhs);