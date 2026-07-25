#pragma once

#include "../compiler/context.h"
#include "../util/container.h"

namespace ast {
    struct Decl;
    struct Type;
    struct Arg;
    struct ParseRegion;
}

struct Module;
struct Function;
struct DerivedTypes;
struct FunType;
struct RecordType;
struct Field;
struct GlobalRegion;
struct Type;

using GlobalBase = RegionBase<GlobalRegion>;

template<class T>
using GlobalPtr = RegionPtr<GlobalRegion, T>;

using TypePtr = RegionPtr<GlobalRegion, Type>;

template<class T>
using GlobalList = SmallList<GlobalRegion, T, false>;

struct Limits {
    static const U32 maxTypeDescriptor = 2048;
};

struct Type {
    enum Kind: U8 {
        Error,
        Unit,
        Gen,
        Int,
        Float,
        String,
        Ptr,
        Ref,
        Fun,
        Array,
        Map,
        Tup,
        Record,
        Alias,
    };

    void* codegen = nullptr;

    // We store a reference to some of the types that reference a single type.
    // This is an efficient way to make sure that only a single instance is created.
    GlobalPtr<DerivedTypes> derived = nullptr;

    // The type descriptor. This is a globally unique descriptor for each type -
    // if the descriptors are the same, the types are the same.
    // Additionally, the descriptor can be reversed into the type it was created from,
    // allowing for easy serialization.
    GlobalPtr<Byte> descriptor = nullptr;
    U16 descriptorLength = 0;

    // The amount of virtual space this type requires.
    // This is used for compile-time evaluation and storing constants in a platform-independent way.
    U16 virtualSize;
    Kind kind;

    Type(Kind kind, U32 virtualSize): kind(kind), virtualSize(virtualSize) {}
};

struct ErrorType: Type {
    ErrorType(): Type(Error, 0) {}
};

struct UnitType: Type {
    UnitType(): Type(Unit, 0) {}
};

struct StringType: Type {
    StringType(): Type(String, 1) {}
};

struct GenType: Type {
    GenType(StringId name, U32 index):
        Type(Gen, 1) {}

    GlobalList<TypePtr> args;
    StringId name;
    U32 index;
};

struct IntType: Type {
    enum Width: U8 {
        Bool,
        Int,
        Long,
    };

    static constexpr U32 bitsForWidth(Width w) {
        switch(w) {
            case Bool: return 1;
            case Int: return 32;
            case Long: return 64;
            default: return 0;
        }
    }

    IntType(U16 bits, Width width, bool isSigned):
        Type(Kind::Int, 1), bits(bits), width(width), isSigned(isSigned) {}

    U16 bits;
    Width width;
    bool isSigned;
};

struct FloatType: Type {
    enum Width: U8 {
        F16,
        F32,
        F64,
    };

    static constexpr U32 bitsForWidth(Width w) {
        switch(w) {
            case F16: return 16;
            case F32: return 32;
            case F64: return 64;
        }

        return 0;
    }

    FloatType(U16 bits, Width width): Type{Float, 1}, bits(bits), width(width) {}

    U16 bits;
    Width width;
};

struct PtrType: Type {
    PtrType(TypePtr to): Type(Ptr, 1), to(to) {}
    TypePtr to;
};

struct RefType: Type {
    RefType(TypePtr to): Type(Ref, 2), to(to) {}
    TypePtr to;
};

struct FunArg {
    TypePtr type;
    StringId name;
};

struct FunType: Type {
    FunType(): Type(Fun, 2) {}

    GlobalList<FunArg> args;
    TypePtr result = nullptr;
};

struct ArrayType: Type {
    ArrayType(TypePtr content): Type(Array, 2), content(content) {}
    TypePtr content;
};

struct MapType: Type {
    MapType(TypePtr from, TypePtr to): Type(Map, 2), from(from), to(to) {}
    TypePtr from, to;
};

struct Field {
    TypePtr type;
    TypePtr container;
    StringId name;
};

struct TupType: Type {
    TupType(U32 virtualSize): Type(Tup, virtualSize) {}

    GlobalList<Field> fields;
    bool named;
};

struct Con {
    void* codegen = nullptr;

    GlobalPtr<RecordType> parent;
    TypePtr content;

    StringId name;
    U32 index: 31;
    bool exported: 1;
};

struct RecordType: Type {
    enum Kind: U8 {
        // An enum record acts as a single int type.
        Enum,

        // A single constructor record acts as the type in its constructor.
        Single,

        // A multi-constructor record acts as two fields - the constructor id and data.
        Multi,
    };

    RecordType(): Type(Record, 0), kind(Multi) {}

    // Set until the type is fully resolved.
    RegionPtr<ast::ParseRegion, ast::Decl> ast = nullptr;

    GlobalList<Con> cons;
    StringId name;
    Kind kind;

    // Set if the type constructors are namespaced within the type.
    bool qualified;
};

struct AliasType: Type {
    AliasType(StringId name, TypePtr to):
        Type(Alias, 0), to(to), name(name) {}

    // Set until the type is fully resolved.
    RegionPtr<ast::ParseRegion, ast::Decl> ast = nullptr;

    TypePtr to;
    StringId name;
};

struct DerivedTypes {
    explicit DerivedTypes(Module* module, Type* type);

    PtrType ptr;
    ArrayType arrayTo;
};

// Returns a reference to the provided type.
Type* getPtr(Module* module, Type* to);

// Returns an array type of the provided type.
Type* getArray(Module* module, Type* to);

// Finishes the definition of a type defined in the module, if needed.
Type* resolveDefinition(Context* context, Module* module, Type* type);

// Finds the matching type for the provided ast.
Type* resolveType(Context* context, Module* module, ast::Type* type);

// Finds a tuple type with these field types and names.
// If none existed, a type is created with the fields copied.
TupType* resolveTupType(Context* context, Module* module, Field* fields, U32 count);

// Checks if the two provided types are the same.
bool compareTypes(Context* context, Type* lhs, Type* rhs);

// Returns the canonical type of this type - the IR-level when discarding alias information.
Type* canonicalType(Type* type);

// Returns the type this type acts as when used as an rvalue.
Type* rValueType(Type* type);

// Generates the descriptor for a newly built type.
void createDescriptor(Type* type, Arena* arena);

// Returns the symbol name of a type. Returns 0 if the type is not named.
// Named types are explicitly defined in some module and can be found by that name.
StringId typeName(Type* type);

// Creates a set of the generic type names used in a context.
void findGenerics(Context* context, Buffer<StringId> buffer, Size& offset, ast::Type* type);

// When instantiating types, we add each alias and record to a stack.
// If it turns out that the current type is already on the stack with the same arguments (the type is recursive),
// we use the existing reference instead.
struct RecordEntry {
    RecordEntry* prev;
    RecordType* type;
};

// Calls a visitor callback for each type referenced inside the provided one.
template<class F> void visitType(GlobalBase base, Type* type, F&& f) {
    switch(type->kind) {
        case Type::Error:
            f((ErrorType*)type);
            break;
        case Type::Unit:
            f((UnitType*)type);
            break;
        case Type::Gen:
            f((GenType*)type);
            break;
        case Type::Int:
            f((IntType*)type);
            break;
        case Type::Float:
            f((FloatType*)type);
            break;
        case Type::String:
            f((StringType*)type);
            break;
        case Type::Ptr:
            f((PtrType*)type);
            visitType(((PtrType*)type)->to, forward<F>(f));
            break;
        case Type::Fun: {
            auto fun = (FunType*)type;
            f(fun);
            visitType(fun->result, forward<F>(f));

            for(auto a: fun->args.contents(base)) {
                visitType(base, base[a.type], forward<F>(f));
            }
            break;
        }
        case Type::Array:
            f((ArrayType*)type);
            visitType(((ArrayType*)type)->content, forward<F>(f));
            break;
        case Type::Map:
            f((MapType*)type);
            visitType(((MapType*)type)->from, forward<F>(f));
            visitType(((MapType*)type)->to, forward<F>(f));
            break;
        case Type::Tup: {
            auto tup = (TupType*)type;
            f(tup);

            for(auto a: tup->fields.contents(base)) {
                visitType(base, base[a.type], forward<F>(f));
            }
            break;
        }
        case Type::Record: {
            auto record = (RecordType*)type;
            f(record);

            for(auto a: record->cons.contents(base)) {
                visitType(base, base[a.content], forward<F>(f));
            }
            break;
        }
        case Type::Alias:
            visitType(((AliasType*)type)->to, forward<F>(f));
            break;
    }
}
