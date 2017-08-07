#pragma once

#include "../compiler/context.h"

namespace ast { struct AliasDecl; struct DataDecl; struct ClassDecl; struct Type; }

struct Module;
struct Function;
struct TypeClass;

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

struct GenField {
    Type* type;
    struct GenType* gen;
    Id name;
    bool mut;
};

struct GenType: Type {
    GenType(U32 index): Type(Gen), index(index) {}

    GenField* fields; // A list of fields this type must contain.
    TypeClass** classes; // A list of classes this type must implement.
    U32 index;
    U16 fieldCount = 0;
    U16 classCount = 0;
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

    RecordType(): Type(Record), kind(Multi) {}

    ast::DataDecl* ast; // Set until the type is fully resolved.
    Con* cons;
    Type** gens;
    Id name;
    U16 conCount;
    U16 genCount: 13;
    Kind kind: 2;
    bool qualified: 1; // Set if the type constructors are namespaced within the type.
};

struct AliasType: Type {
    AliasType(): Type(Alias) {}

    ast::AliasDecl* ast; // Set until the type is fully resolved.
    Type** gens;
    Type* to;
    Id name;
    U32 genCount;
};

struct TypeClass {
    ast::ClassDecl* ast; // Set until the type is fully resolved.

    GenType** args; // A list of types this class will be instantiated on.
    FunType** functions; // A list of function types this class implements.
    Id name;
    U16 argCount = 0;
    U16 funCount = 0;
};

struct ClassInstance {
    Module* module;
    TypeClass* typeClass;
    Type** forTypes; // A list of instance args for the class. Has the same length as typeClass->args.
    Function** instances; // A list of function implementations. Corresponds to the list in typeClass->functions.
};

struct InstanceLookup {
    ClassInstance instance;
    HashMap<InstanceLookup, Size> next;
    U32 depth = 0;
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

// Returns the canonical type of this type - the base type it acts like when used.
Type* canonicalType(Type* type);