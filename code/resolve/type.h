#pragma once

#include "../compiler/context.h"

namespace ast { struct AliasDecl; struct DataDecl; struct ClassDecl; struct Type; }

struct Module;
struct Function;
struct TypeClass;
struct DerivedTypes;

struct Limits {
    static const U32 maxTypeDescriptor = 2048;
};

struct Type {
    enum Kind {
        Error,
        Unit,
        Gen,
        Int,
        Float,
        String,
        Ref,
        Fun,
        Array,
        Map,
        Tup,
        Record,
        Alias,
    };

    void* codegen = nullptr;

    // We store a reference to some of the of types that reference a single type.
    // This is an efficient way to make sure that only a single instance is created.
    DerivedTypes* derived = nullptr;

    // The type descriptor. This is a globally unique descriptor for each type -
    // if the descriptors are the same, the types are the same.
    // Additionally, the descriptor can be reversed into the type it was created from,
    // allowing for easy serialization.
    const Byte* descriptor;
    U16 descriptorLength = 0;

    // The amount of virtual space this type requires.
    // This is used for compile-time evaluation and storing constants in a platform-independent way.
    U16 virtualSize;
    Kind kind;

    Type(Kind kind, U32 virtualSize): kind(kind), virtualSize(virtualSize) {}
};

struct GenField {
    Type* type;
    struct GenType* gen;
    Id name;
    bool mut;
};

struct GenType: Type {
    GenType(Id name, U32 index): Type(Gen, 1), name(name), index(index) {}

    GenField* fields; // A list of fields this type must contain.
    TypeClass** classes; // A list of classes this type must implement.
    Id name;
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

    IntType(U16 bits, Width width): Type(Kind::Int, 1), bits(bits), width(width) {}

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

    FloatType(U16 bits, Width width): Type{Float, 1}, bits(bits), width(width) {}

    U16 bits;
    Width width;
};

// A reference to a value. A reference can be traced, untraced or local, as well as mutable/immutable.
// The code generator should handle some implicit conversions that won't affect the generator code in most cases:
//  - A mutable reference can be implicitly converted to an immutable one.
//  - An untraced reference can be implicitly converted to a local one.
// All other cases are handled explicitly in the IR.
struct RefType: Type {
    RefType(Type* to, bool isTraced, bool isLocal, bool isMutable):
        Type(Ref, 1), to(to), isTraced(isTraced), isLocal(isLocal), isMutable(isMutable) {}

    Type* to;

    // A traced reference points to the heap and is managed by the GC.
    // Loads and stores may need a read/write barrier depending on the GC used.
    // An untraced reference can point anywhere, but is considered unsafe if it is not also local.
    // Unsafe references cannot be used when compiling to JS.
    bool isTraced;

    // Local references point to a value on the stack. They can be sent upwards on the stack,
    // but cannot be returned or stored in non-local references.
    // A local reference is never traced.
    bool isLocal;

    // If a reference is mutable, the location it points to can be stored into.
    // Otherwise, if can only be loaded.
    // This is more than just a flag, since mutable references may be stored in a different heap than immutable ones.
    bool isMutable;
};

struct FunArg {
    Type* type;
    Id name;
    U32 index;
};

struct FunType: Type {
    FunType(): Type(Fun, 2) {}

    FunArg* args;
    Type* result;
    U32 argCount;
};

struct ArrayType: Type {
    ArrayType(Type* content): Type(Array, 2), content(content) {}
    Type* content;
};

struct MapType: Type {
    MapType(Type* from, Type* to): Type(Map, 2), from(from), to(to) {}
    Type* from, *to;
};

struct Field {
    Type* type;
    Type* container;
    Id name;
    U32 index;
};

struct TupType: Type {
    TupType(U32 virtualSize): Type(Tup, virtualSize) {}
    Field* fields;
    U32 count;
    bool named;
};

struct Con {
    struct RecordType* parent;
    Field* fields;
    Id name;
    U32 index;
    U32 count;
    bool exported;
};

struct RecordType: Type {
    enum Kind {
        // An enum record acts as a single int type.
        Enum,

        // A single constructor record acts as the type in its constructor.
        Single,

        // A multi-constructor record acts as two fields - the constructor id and data.
        Multi,
    };

    RecordType(): Type(Record, 0), kind(Multi) {}

    ast::DataDecl* ast; // Set until the type is fully resolved.
    Con* cons;
    GenType* gens;
    RecordType* instanceOf;
    Id name;
    U32 conCount;
    U32 genCount;
    Kind kind;
    bool qualified; // Set if the type constructors are namespaced within the type.
};

struct AliasType: Type {
    AliasType(): Type(Alias, 0) {}

    ast::AliasDecl* ast; // Set until the type is fully resolved.
    GenType* gens;
    Type* to;
    AliasType* instanceOf;
    Id name;
    U32 genCount;
};

struct TypeClass {
    ast::ClassDecl* ast; // Set until the type is fully resolved.

    GenType* args; // A list of types this class will be instantiated on.
    FunType* functions; // A list of function types this class implements.
    Id* funNames; // The name of each class function, in order.

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

struct DerivedTypes {
    explicit DerivedTypes(Module* module, Type* type);

    RefType tracedMutableRef;
    RefType tracedImmutableRef;
    RefType localMutableRef;
    RefType localImmutableRef;
    RefType untracedRef;
    ArrayType arrayTo;
};

struct GenContext {
    GenContext* parent;
    GenType* types;
    U32 count;
};

// Global instances of the basic builtin types.
extern Type unitType;
extern Type errorType;
extern FloatType floatTypes[FloatType::KindCount];
extern IntType intTypes[IntType::KindCount];
extern Type stringType;

// Returns a reference to the provided type.
Type* getRef(Module* module, Type* to, bool traced, bool local, bool mut);

// Returns an array type of the provided type.
Type* getArray(Module* module, Type* to);

// Finishes the definition of a type defined in the module, if needed.
Type* resolveDefinition(Context* context, Module* module, Type* type);

// Finds the matching type for the provided ast.
Type* resolveType(Context* context, Module* module, ast::Type* type, GenContext* gen);

// Finds a tuple type with these field types and names.
// If none existed, a type is created with the fields copied.
TupType* resolveTupType(Context* context, Module* module, Field* fields, U32 count);

// Checks if the two provided types are the same.
bool compareTypes(Context* context, Type* lhs, Type* rhs);

// Returns the canonical type of this type - the base type it acts like when used.
Type* canonicalType(Type* type);