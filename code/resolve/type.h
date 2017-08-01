#pragma once

#include "../compiler/context.h"

struct Module;
struct Function;

struct Type {
    enum Kind {
        Error,
        Gen,
        Int,
        Float,
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
};

struct TypeClass {
    Id name;
    Array<struct GenType*> parameters;
    Array<struct FunType*> functions;
};

struct ClassInstance {
    Module* module;
    TypeClass* typeClass;
    Type* forType;
    HashMap<FunType*, Function*> implementations;
};

struct GenField {
    Type* type;
    struct GenType* gen;
    Id name;
    bool mut;
};

struct GenType: Type {
    U32 index;
    Array<GenField> fields;
    Array<TypeClass*> classes;
};

struct IntType: Type {
    enum Kind: U8 {
        Bool,
        Int,
        Long,
        KindCount,
    };

    U16 width;
    U8 kind;
};

struct FloatType: Type {
    enum Kind {
        F16,
        F32,
        F64,
        KindCount
    };

    Kind kind;
};

struct RefType: Type {
    Type* to;
};

struct PtrType: Type {
    Type* to;
};

struct FunArg {
    Type* type;
    Id name;
    U32 index;
};

struct FunType: Type {
    Array<FunArg> args;
    Type* result;
};

struct ArrayType: Type {
    Type* content;
};

struct MapType: Type {
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
    Array<Field> fields;
    Array<Type*> layout;
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

    struct DataDecl* ast; // Set until the type is fully resolved.
    Array<Con> cons;
    Array<Type*> gens;
    Id name;
    Kind kind;
};

struct AliasType: Type {
    struct TypeDecl* ast; // Set until the type is fully resolved.
    Array<Type*> gens;
    Type* to;
    Id name;
};

extern Type unitType;
extern FloatType floatTypes[FloatType::KindCount];
extern IntType intTypes[IntType::KindCount];
extern Type stringType;
