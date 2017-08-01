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

struct RefType: Type {
    RefType(Type* to): Type(Ref), to(to) {}
    Type* to;
};

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
    Id name;
    U32 index;
    Type* type;
    Type* container;
    bool mut;
};

struct TupType: Type {
    TupType(): Type(Tup) {}
    ::Array<Field> fields;
    ::Array<Type*> layout;
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

// Finishes the definition of a type defined in the module, if needed.
Type* resolveDefinition(Module* module, Type* type);

// Finds the matching type for the provided ast.
Type* resolveType(Module* module, ast::Type* type);