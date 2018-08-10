#pragma once

#include "../compiler/context.h"

namespace ast {
    struct AliasDecl;
    struct DataDecl;
    struct ClassDecl;
    struct FunDecl;
    struct Type;
    struct FunType;
    struct Arg;
}

struct Module;
struct Function;
struct TypeClass;
struct DerivedTypes;
struct FunType;
struct GenType;
struct GenEnv;
struct ClassInstance;
struct RecordType;
struct Field;

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

struct ErrorType: Type {
    ErrorType(): Type(Error, 0) {}
};

struct UnitType: Type {
    UnitType(): Type(Unit, 0) {}
};

struct StringType: Type {
    StringType(): Type(String, 1) {}
};

/*
 * Generic type resolving.
 * - Generic types are fully defined types owned by a specific generic environment.
 *   Generic environments can be functions, typeclasses, records, etc.
 * - Generic environments are used for both type checking and code generation.
 *   The CallGen and CallDynGen instructions explicitly pass (parts of) these environments,
 *   allowing code generation to use platform-specific representations.
 * - A generic type is only valid if used inside the containing environment.
 *   This means that passing generic types between functions requires explicit instructions to convert the type to that environment.
 * - A generic type can be a generic instantiation of another type. This allows representing higher-kinded types.
 */

struct GenField {
    ast::Type* ast;

    Type* fieldType;
    GenType* container;
    Id fieldName;
    bool mut;
};

struct GenFun {
    ast::FunType* ast;

    FunType* type;
    Id name;
};

struct GenType: Type {
    GenType(GenEnv* env, Id name, U32 index):
        Type(Gen, 1), env(env), name(name), index(index) {}

    GenEnv* env;
    Type* orderType = nullptr; // Forms a list defining the order of this type.
    Id name;
    U16 index;
    U16 order = 0; // 0 indicates undetermined.
};

struct ClassConstraint {
    Id ast;
    TypeClass* classType;
    Buffer<GenType*> forTypes;
};

struct GenEnv {
    enum Kind: U8 {
        Record,
        Class,
        Instance,
        Function,
        Alias,
    };

    GenEnv(void* container, Kind kind): container(container), kind(kind) {}

    GenEnv* parent = nullptr;
    void* container; // Type is defined by `kind`.
    GenType** types; // List of generic types used in this environment.
    GenField* fields; // A list of fields used in this environment.
    ClassConstraint* classes; // A list of typeclass constraints that apply to this environment.
    GenFun* funs; // A list of functions targeting generic types used in this environment.

    U16 typeCount = 0;
    U16 fieldCount = 0;
    U16 classCount = 0;
    U16 funCount = 0;
    Kind kind;
};

struct GenInstance {
    struct TypeInstance {
        union {
            Type* type;
            GenType* gen;
        };

        bool isComplete;
    };

    struct FieldInstance {
        union {
            Field* field;
            GenField* gen;
        };

        bool isComplete;
    };

    struct ConstraintInstance {
        union {
            ClassInstance* instance;
            ClassConstraint* gen;
        };

        bool isComplete;
    };

    struct FunInstance {
        union {
            Function* fun;
            GenFun* gen;
        };

        bool isComplete;
    };

    GenEnv* sourceEnv;
    GenEnv* targetEnv;

    // Each constraint from the target env is mapped to either a source env constraint or a complete implementation.
    TypeInstance* types;
    FieldInstance* fields;
    ConstraintInstance* classes;
    FunInstance* funs;
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
    RecordType* parent;
    Type* content;

    void* codegen = nullptr;

    Id name;
    U32 index;
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

    RecordType(): Type(Record, 0), kind(Multi), gen(this, GenEnv::Record) {}

    ast::DataDecl* ast; // Set until the type is fully resolved.
    Con* cons;
    GenEnv gen;
    GenType** args;
    RecordType* instanceOf;
    Type** instance; // if instanceOf is set, this contains a list Type*[instanceOf->argCount].
    Id name;
    U32 conCount;
    U32 argCount;
    Kind kind;
    bool qualified; // Set if the type constructors are namespaced within the type.
};

struct AliasType: Type {
    AliasType(): Type(Alias, 0), gen(this, GenEnv::Alias) {}

    ast::AliasDecl* ast; // Set until the type is fully resolved.
    Type* to;
    GenEnv gen;
    GenType** args;
    AliasType* instanceOf;
    Id name;
    U32 argCount;
};

struct ClassFun {
    Function* fun;
    TypeClass* typeClass;
    U32 index;
    Id name;
};

/*
 * Typeclasses support generic arguments on multiple levels, and as such need careful handling to prevent compiler bugs.
 * Because of this we have decided to store and handle class arguments separately from the generic environment they are defined in.
 * This prevents any confusion regarding what types are actually input arguments, both for current and any future features.
 *
 * Classes are generally quite complicated. Consider the following example:
 *
 * class Functor(f):
 *   fn map(functor: f(a), apply: (a) -> b) -> f(b)
 *
 * In this case, the class argument _f_ is used as a second-order type in the class definition.
 * However, the initial resolve pass only adds _f_ to the environment as an empty generic type.
 * Once the function signatures are being resolved, the generic environment needs to be updated with the correct order.
 * Additionally, once an order for the type argument is defined,
 * it has to be ensured that any other uses are valid taking into account the type order.
 *
 * Now consider an additional example:
 *
 * instance Functor(Maybe):
 *   fn map(maybe: Maybe(a), apply: (a) -> b) -> Maybe(b) = match maybe:
 *     Just(v) -> Just(apply(v))
 *     Nothing -> Nothing
 *
 * In this case, we have to implicitly map the _a_ argument from the _Maybe(a)_ environment, to the _a_ argument in the _Functor_ environment.
 * Additionally, we have to make sure to correctly map f(a) to Maybe(a) in the function implementation.
 * This get more complicated when we add an explicit specialization:
 *
 * instance Functor(Maybe(%a)):
 *   fn map(maybe: Maybe(%a), apply: (%a) -> b) -> Maybe(b) = ...code...
 *
 * First of all, note that we cannot change the mapping function return type _b_ to some more-defined type like _%b_.
 * This would violate the base Functor definition, which indicates that _map_ has to handle any return type _b_.
 * This limits the usefulness of this particular specialization, and since _b_ is just a function argument rather
 * than a class one, it is currently not possible to specialize any more.
 *
 * Secondly, supporting constructions like this one requires quite a sophisticated matching of class function class to implementations.
 * We cannot just compare type signatures - we also have to handle matching any generic arguments to implementations.
 */
struct TypeClass {
    TypeClass(): gen(this, GenEnv::Class) {}

    ast::ClassDecl* ast; // Set until the type is fully resolved.
    GenEnv gen; // The generic environment for the type arguments of this class.
    GenType** args; // The type arguments this class takes. Defined in the generic environment together with any constraints.
    ClassFun* functions; // The function signatures defined in this class.
    Id name;
    U16 argCount; // The number of arguments in _args_.
    U16 funCount; // The number of functions in _functions_.
};

struct ClassInstance {
    Module* module;
    TypeClass* typeClass;
    Type** forTypes; // A list of instance args for the class, corresponding to the types in its generic environment.
    Function** instances; // A list of function implementations. Corresponds to the class function list.
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

// Global instances of the basic builtin types.
extern UnitType unitType;
extern ErrorType errorType;
extern FloatType floatTypes[FloatType::KindCount];
extern IntType intTypes[IntType::KindCount];
extern StringType stringType;

// Returns a reference to the provided type.
Type* getRef(Module* module, Type* to, bool traced, bool local, bool mut);

// Returns an array type of the provided type.
Type* getArray(Module* module, Type* to);

// Finishes the definition of a type defined in the module, if needed.
Type* resolveDefinition(Context* context, Module* module, Type* type);

// Finds the matching type for the provided ast.
Type* resolveType(Context* context, Module* module, ast::Type* type, GenEnv* env);

// Finds a tuple type with these field types and names.
// If none existed, a type is created with the fields copied.
TupType* resolveTupType(Context* context, Module* module, Field* fields, U32 count);

// Performs final resolving of types on a prepared generic environment.
void resolveGens(Context* context, Module* module, GenEnv* env);

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
Id typeName(Type* type);

// Creates a set of the generic type names used in a context.
void findGenerics(Context* context, Buffer<Id> buffer, Size& offset, ast::Type* type);

// When instantiating types, we add each alias and record to a stack.
// If it turns out that the current type is already on the stack with the same arguments (the type is recursive),
// we use the existing reference instead.
struct RecordEntry {
    RecordEntry* prev;
    RecordType* type;
};

// Instantiates a higher-order type for a specific set of arguments.
// Returns a new type which is distinct from but references the original.
// If direct is set, the action represents an explicit instantiation of this type.
// If not, the action represents an implicit instantiation through a containing type.
AliasType* instantiateAlias(Context* context, Module* module, AliasType* type, Type** args, U32 count, RecordEntry* entries, bool direct);
RecordType* instantiateRecord(Context* context, Module* module, RecordType* type, Type** args, U32 count, RecordEntry* entries, bool direct);

// Calls a visitor callback for each type referenced inside the provided one.
template<class F> void visitType(Type* type, F&& f) {
    switch(type->kind) {
        case Type::Error:
            f((ErrorType*)type);
            break;
        case Type::Unit:
            f((UnitType*)type);
            break;
        case Type::Gen: {
            auto gen = (GenType*)type;
            f(gen);
            if(gen->orderType) visitType(gen->orderType, forward<F>(f));
            break;
        }
        case Type::Int:
            f((IntType*)type);
            break;
        case Type::Float:
            f((FloatType*)type);
            break;
        case Type::String:
            f((StringType*)type);
            break;
        case Type::Ref:
            f((RefType*)type);
            visitType(((RefType*)type)->to, forward<F>(f));
            break;
        case Type::Fun: {
            auto fun = (FunType*)type;
            f(fun);
            visitType(fun->result, f);
            for(U32 i = 0; i < fun->argCount; i++) {
                visitType(fun->args[i].type, f);
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
            for(U32 i = 0; i < tup->count; i++) {
                visitType(tup->fields[i].type, forward<F>(f));
            }
            break;
        }
        case Type::Record: {
            auto record = (RecordType*)type;
            f(record);
            for(U32 i = 0; i < record->conCount; i++) {
                visitType(record->cons[i].content, forward<F>(f));
            }
            break;
        }
        case Type::Alias:
            visitType(((AliasType*)type)->to, forward<F>(f));
            break;
    }
}