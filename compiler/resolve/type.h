#pragma once

#include "../compiler/context.h"
#include "../parse/ast.h"
#include "../util/container.h"

struct Module;
struct GlobalRegion {};

using GlobalBase = RegionBase<GlobalRegion>;

template<class T>
using GlobalPtr = RegionPtr<GlobalRegion, T>;

template<class T>
using GlobalList = SmallList<GlobalRegion, T, false>;

struct Type;
using TypePtr = GlobalPtr<Type>;

struct Repr {
    U32 size = 0;
    U32 align = 1;
};

// Resolve types are interned in a module's global region. Repr is deliberately limited
// to the size/alignment/offset information needed by Milestone 2; packing and niches are
// later passes.
struct Type {
    enum Kind: U8 {
        Error,
        Unit,
        Int,
        Float,
        Borrow,
        Ref,
        RegionPtr,
        Ptr,
        Region,
        Fun,
        Array,
        Map,
        Tup,
        Record,
        Alias,
        Gen,
    };

    Type(Kind kind, U16 virtualSize, Repr repr = {}):
        virtualSize(virtualSize), repr(repr), kind(kind) {}

    GlobalPtr<Byte> descriptor = nullptr;
    U16 descriptorLength = 0;
    U16 virtualSize;
    Repr repr;
    Kind kind;
};

struct IntType: Type {
    enum Width: U8 {
        Bool,
        Int,
        Long,
    };

    IntType(U16 bits, Width width, bool isSigned):
        Type(Type::Int, 1, { U32(bits / 8), U32(bits / 8) }),
        bits(bits), width(width), isSigned(isSigned) {}

    U16 bits;
    Width width;
    bool isSigned;
};

struct FloatType: Type {
    enum Width: U8 {
        Float,
        Double,
    };

    explicit FloatType(Width width):
        Type(Type::Float, 1, {
            width == Float ? 4u : 8u,
            width == Float ? 4u : 8u,
        }), width(width) {}

    Width width;
};

struct Field {
    TypePtr type = nullptr;
    StringId name = 0;
    U32 offset = 0;
};

struct TupType: Type {
    TupType(): Type(Type::Tup, 0) {}

    GlobalList<Field> fields;
    bool named = false;
    bool resolvingRepr = false;
    bool reprReady = false;
};

struct Constructor {
    StringId name = 0;
    TypePtr content = nullptr;
    U32 index = 0;
};

struct RecordType: Type {
    enum Layout: U8 {
        Enum,
        Single,
        Multi,
    };

    explicit RecordType(StringId name):
        Type(Type::Record, 0), name(name) {}

    GlobalList<Constructor> constructors;
    StringId name;
    U32 payloadOffset = 0;
    Layout layout = Multi;
    bool qualified = false;
    bool resolvingRepr = false;
    bool definitionReady = false;
    bool reprReady = false;
};

struct ConstructorRef {
    GlobalPtr<RecordType> record = nullptr;
    U16 index = 0;
};

struct ScalarTypes {
    TypePtr error = nullptr;
    TypePtr unit = nullptr;
    TypePtr bool_ = nullptr;
    TypePtr int_ = nullptr;
    TypePtr long_ = nullptr;
    TypePtr float_ = nullptr;
    TypePtr double_ = nullptr;
};

TypePtr resolveType(Context& context, Module& module, const ast::Type& type);
TupType* resolveTupleType(Context& context, Module& module, Buffer<Field> fields, LocationId source);

bool finishTupleRepr(Context& context, Module& module, TupType& tuple, LocationId source);
bool finishRecordRepr(Context& context, Module& module, RecordType& record, LocationId source);

bool sameType(TypePtr lhs, TypePtr rhs);
bool isUnit(GlobalBase base, TypePtr type);
bool isBool(GlobalBase base, TypePtr type);
bool isInteger(GlobalBase base, TypePtr type);
bool isFloat(GlobalBase base, TypePtr type);
bool isNumeric(GlobalBase base, TypePtr type);
bool isDirectType(GlobalBase base, TypePtr type);
bool isMemoryType(GlobalBase base, TypePtr type);

U32 typeSize(GlobalBase base, TypePtr type);
U32 typeAlign(GlobalBase base, TypePtr type);
StringView typeName(GlobalBase base, TypePtr type);

// How a type is written in a diagnostic. Unlike typeName() this can name a record, which needs
// the Context its name was interned in.
String describeType(Context& context, GlobalBase base, TypePtr type);
