#pragma once

#include "../compiler/context.h"
#include "../parse/ast.h"
#include "../util/container.h"

struct Program;
struct Module;
struct GlobalRegion {};

using GlobalBase = RegionBase<GlobalRegion>;

template<class T>
using GlobalPtr = RegionPtr<GlobalRegion, T>;

template<class T>
using GlobalList = SmallList<GlobalRegion, T, false>;

struct Type;
struct GenEnv;
struct GenType;
struct TypeClass;
using TypePtr = GlobalPtr<Type>;

struct Repr {
    U32 size = 0;
    U32 align = 1;
};

// Resolve types live in one region shared by every module of a program, so that a type
// resolved in Core is the same TypePtr when a user module names it. Interning is what makes
// that identity meaningful: sameType() is pointer equality, and instance selection, generic
// instantiation caching and Repr all depend on it.
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
        Gen,
    };

    Type(Kind kind, U16 virtualSize, Repr repr = {}):
        virtualSize(virtualSize), repr(repr), kind(kind) {}

    GlobalPtr<Byte> descriptor = nullptr;
    U16 descriptorLength = 0;
    U16 virtualSize;
    Repr repr;
    Kind kind;

    // Set when a type variable is reachable inside this type. A generic type has no Repr and
    // never reaches the IR; it exists to be substituted.
    bool generic = false;
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

/*
 * Generic contexts.
 *
 * A generic type variable belongs to exactly one context - the declaration that introduced it -
 * rather than being ambient, which is what lets `Serialize(type, target)`-shaped constraints
 * relate two variables of the same context (Design.md's "Resolving"). Milestone 1 builds these
 * contexts for `data`, `alias`, `class` and `instance` declarations; function contexts and the
 * requirement collection that goes with them are Milestone 2.
 */

struct GenType: Type {
    GenType(GlobalPtr<GenEnv> env, StringId name, U16 index):
        Type(Type::Gen, 1), env(env), name(name), index(index) { generic = true; }

    GlobalPtr<GenEnv> env;
    StringId name;
    U16 index;
};

// One `Class(a, b)` requirement of a context. `args` are the context's own types (or concrete
// types, for a partially applied constraint), in the class's argument order.
struct ClassConstraint {
    GlobalPtr<TypeClass> typeClass = nullptr;
    GlobalList<TypePtr> args;
    StringId name = 0;
    LocationId source = kNullLocation;
};

struct GenEnv {
    enum Kind: U8 {
        Record,
        Alias,
        Class,
        Instance,
        Function,
    };

    explicit GenEnv(Kind kind): kind(kind) {}

    GlobalList<GlobalPtr<GenType>> types;
    GlobalList<ClassConstraint> classes;
    Kind kind;
};

struct RecordType: Type {
    enum Layout: U8 {
        Enum,
        Single,
        Multi,
    };

    explicit RecordType(StringId name):
        Type(Type::Record, 0), name(name) {}

    // The declaration this type came from: itself for a plain or generic declaration, and the
    // generic declaration for one of its instantiations.
    GlobalPtr<RecordType> base(GlobalBase global) {
        return instanceOf ? instanceOf : (RecordType*)this - global;
    }

    GlobalList<Constructor> constructors;
    StringId name;

    // Set on a generic declaration: its type variables, and the instantiations made from it.
    GlobalPtr<GenEnv> gen = nullptr;
    GlobalList<GlobalPtr<RecordType>> instances;

    // Set on an instantiation: what it was made from, and with which concrete arguments.
    GlobalPtr<RecordType> instanceOf = nullptr;
    GlobalList<TypePtr> instanceArgs;

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

    explicit operator bool() const { return record != nullptr; }
};

// A generic `alias` declaration. Aliases are transparent - resolving one substitutes straight
// through to the target type - so they are not a Type kind and never reach the IR.
//
// The target is kept as AST and resolved on first use, so an alias may name a type declared
// after it. `module` is where that resolution happens: an alias reached through an import
// resolves its target in the module that wrote it, not in the one that named it.
struct TypeAlias {
    StringId name = 0;
    Module* module = nullptr;
    GlobalPtr<GenEnv> gen = nullptr;
    ast::ParsePtr<ast::Decl> ast = nullptr;
    TypePtr resolved = nullptr;
    LocationId source = kNullLocation;
    bool resolving = false;
};

struct ScalarTypes {
    TypePtr error = nullptr;
    TypePtr unit = nullptr;
    TypePtr bool_ = nullptr;
    TypePtr int_ = nullptr;
    TypePtr long_ = nullptr;
    TypePtr float_ = nullptr;
    TypePtr double_ = nullptr;
    TypePtr ordering = nullptr;
};

/*
 * Resolving types.
 *
 * `env` is the generic context the type is written in, if any: it is what makes a lowercase
 * name in a declaration resolve to that declaration's own type variable rather than being an
 * error. A null env means no type variable is in scope.
 */
TypePtr resolveType(Module& module, const ast::Type& type, GenEnv* env = nullptr);
TupType* resolveTupleType(Module& module, Buffer<Field> fields, LocationId source);

// Instantiates a generic record for a set of fully concrete arguments, interning the result so
// that `Maybe(Int)` names one type no matter how many places write it.
TypePtr instantiateRecord(Module& module, GlobalPtr<RecordType> record, Buffer<TypePtr> args, LocationId source);

// Fills in the constructors of every instantiation that was created before the declaration it
// came from had been read. Runs once per module after its data declarations are complete.
void completePendingInstances(Module& module);

// Replaces every type variable of one context with the matching entry of `args`. Used to build
// an instantiation's constructors and to specialize a class method's signature.
TypePtr substituteType(Module& module, TypePtr type, Buffer<TypePtr> args, LocationId source);

// Structural match of a type written against a generic context (`pattern`) with a concrete type,
// binding each type variable it meets in `bindings`. Returns false on a mismatch, including a
// variable that would have to bind to two different types. This is the whole of instance
// selection's inference, and Milestone 2's call-site inference uses the same function.
bool matchType(GlobalBase global, TypePtr pattern, TypePtr concrete, Buffer<TypePtr> bindings);

bool finishTupleRepr(Module& module, TupType& tuple, LocationId source);
bool finishRecordRepr(Module& module, RecordType& record, LocationId source);

bool sameType(TypePtr lhs, TypePtr rhs);
bool isUnit(GlobalBase base, TypePtr type);
bool isInteger(GlobalBase base, TypePtr type);
bool isFloat(GlobalBase base, TypePtr type);
bool isNumeric(GlobalBase base, TypePtr type);
bool isGeneric(GlobalBase base, TypePtr type);
bool isDirectType(GlobalBase base, TypePtr type);
bool isMemoryType(GlobalBase base, TypePtr type);

U32 typeSize(GlobalBase base, TypePtr type);
U32 typeAlign(GlobalBase base, TypePtr type);

// How a type is written in a diagnostic or in printed IR. The buffer form is the one that
// composes; the String form allocates a copy for a diagnostic argument.
void describeType(Context& context, GlobalBase base, TypePtr type, Array<char>& target);
String describeType(Context& context, GlobalBase base, TypePtr type);
void appendText(Array<char>& target, StringView text);
