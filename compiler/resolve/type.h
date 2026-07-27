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

/*
 * What running out of a value's lifetime costs.
 *
 * `None` is nothing at all, `Derived` is the glue the compiler writes - drop each member, then
 * release this owner's own indirect storage - and `Authored` is an `instance Drop(T)` someone
 * wrote. Design.md's Ownership-related classes draws the line where it does because regions need
 * it: derived drop is provably reclaim-only and can be elided when its storage is region-placed,
 * while an authored one cannot be assumed side-effect-free by inspection.
 */
enum class DropKind: U8 {
    None,
    Derived,
    Authored,
};

/*
 * The structural ownership facts of one type (Implementation-IR.md part 4).
 *
 * These are computed from the shape of the type rather than looked up per use, because every one
 * of them is needed long before typeclass dispatch is available: whether a `let x = e` copies or
 * borrows is decided while resolving the `let`, and it is decided by whether `e`'s type is
 * TrivialCopy.
 *
 * `trivialCopy` and `trivialSink` are the two implicit classes and are never spelled in source at
 * this milestone; `authoredCopy` and `drop`/`trivialSink` record what an `instance Copy(T)` /
 * `instance Sink(T)` / `instance Drop(T)` supplied, since those *are* spellable and are what
 * InstCopy and InstMove call. The relations between them are not independent:
 *
 *  - a type with any drop is not TrivialCopy, because copying it would duplicate the resource its
 *    drop releases;
 *  - a type with an authored Sink anywhere inside it is not TrivialSink, because moving it is a
 *    call rather than a memcpy;
 *  - both properties are structural over members, so one non-trivial field is enough.
 */
struct Ownership {
    bool trivialCopy = true;
    bool trivialSink = true;
    bool authoredCopy = false;
    bool authoredSink = false;
    DropKind drop = DropKind::None;
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
        Literal,
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

    // Cached by ownershipOf(). Ownership classification is a whole-program property - one type has
    // one answer - so it is cached on the type the way Repr is, rather than recomputed per module
    // that asks. That relies on instance coherence, which the language already requires.
    Ownership ownership;
    bool ownershipReady = false;
    bool resolvingOwnership = false;
};

/*
 * An integer type.
 *
 * `bits` is how wide the value is in memory and `width` is the primitive it occupies once loaded,
 * which is what Design.md's "integer types can have different sizes when stored in memory,
 * however when loaded they are converted to the closest primitive integer in size" means: `U8`
 * and `I16` are one byte and two bytes of storage but both arrive in a 32-bit register, and only
 * the 64-bit family needs a wider one.
 *
 * `name` is carried rather than derived from `(bits, isSigned)` because Core's `Int` and Native's
 * `I32` are two distinct types of identical shape - separate interned Types with separate class
 * instances - and a diagnostic has to say which one it meant.
 */
struct IntType: Type {
    enum Width: U8 {
        Bool,
        Int,
        Long,
    };

    IntType(U16 bits, Width width, bool isSigned, StringId name = 0):
        Type(Type::Int, 1, { U32(bits / 8), U32(bits / 8) }),
        name(name), bits(bits), width(width), isSigned(isSigned) {}

    StringId name;
    U16 bits;
    Width width;
    bool isSigned;
};

/*
 * A raw pointer - Design.md's `%T`, aliased `Ptr(a)`.
 *
 * Interned on its target type, so that `%Int` written in two places is one TypePtr and pointer
 * equality keeps answering sameType(). A pointer is a direct type: it lives in a register like an
 * Int does, and it is the target address of a load rather than something loaded.
 *
 * The pointee is what pointer arithmetic scales by and what a deref place projects to, so it is
 * kept even though nothing about the machine representation depends on it.
 */
struct PtrType: Type {
    explicit PtrType(TypePtr to):
        Type(Type::Ptr, 1, { 8, 8 }), to(to) {}

    TypePtr to;
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

/*
 * A literal that has not been given a type yet.
 *
 * A literal is a class-polymorphic value (`1` is `FromInt.fromInt(1)`), so the type it ends up
 * with is decided by where it flows rather than by how it is written. Resolving one with no
 * expected type produces a fresh literal variable - printed `?n` - tagged with the classes it
 * has to satisfy, and every position it reaches either binds it to a concrete type or leaves it
 * open for the next one. Whatever is still open when the statement ends takes its class's
 * `default`.
 *
 * `classes` is a list rather than a single class because two literal variables can meet: in
 * `1 + 2.5` the integer literal's FromInt and the decimal literal's FromDecimal are both
 * requirements on one type, and Float is the type that answers both.
 *
 * A literal variable exists only inside one function body's resolution. It never reaches the IR,
 * has no Repr, and is deliberately not interned: two literals written in one expression are two
 * variables even when they end up at the same type.
 */
struct LiteralType: Type {
    explicit LiteralType(U32 index): Type(Type::Literal, 1), index(index) {}

    GlobalList<GlobalPtr<TypeClass>> classes;
    U32 index;
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

/*
 * What one field of a constructor is when a construction leaves it out -
 * `data Flags {read: Bool = False, ...}`.
 *
 * A default is kept as the bits the field's storage would hold rather than as the expression it
 * was written as, for the same reason a global's initializer is (see declareGlobal): there is no
 * program point at which a declaration's code would run, and an expression would additionally
 * belong to the parse arena of the module that wrote it, which is not the one constructing the
 * value. That is what restricts a default to a literal, and it is enough for what the feature is
 * for - a flags type whose fields are all `False`, a counter that starts at zero, a null link.
 *
 * `field` indexes the constructor's content tuple, so only a named field can carry one.
 */
struct FieldDefault {
    U16 field = 0;
    U64 value = 0;
};

struct Constructor {
    StringId name = 0;
    TypePtr content = nullptr;
    U32 index = 0;

    // Only the fields that were given one, in field order; most constructors have none. Read from
    // the declaration rather than from an instantiation of it, since an instantiation can be
    // created before the declaration's defaults have been read - see resolveConstruct.
    GlobalList<FieldDefault> defaults;
};

/*
 * Generic contexts.
 *
 * A generic type variable belongs to exactly one context - the declaration that introduced it -
 * rather than being ambient, which is what lets `Serialize(type, target)`-shaped constraints
 * relate two variables of the same context (Design.md's "Resolving"). `data`, `alias`, `class`
 * and `instance` declarations each get one; a function gets an *open* one, because a function
 * declares its variables by using them rather than in a list of its own.
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

    // A function context has no declared variable list: `fn id(x: a) -> a` introduces `a` by
    // using it. An open context adds a variable the first time a type mentions one, which numbers
    // them in order of appearance across the constraints and then the signature.
    bool open = false;
};

// The type variable of `env` called `name`, adding it if the context is open. Null when the
// context is closed and has no such variable.
GlobalPtr<GenType> genVariable(Module& module, GenEnv& env, StringId name);

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

// The five Core classes the resolver has to know by name rather than by lookup, because the
// language's own syntax is written in terms of them: a literal is a call to one of the first
// two, an implicit conversion is a call to `widen`, and a condition is a call to `truthy`. They
// are ordinary classes in every other respect - user types join them by writing an instance, and
// nothing about selection or instance lookup treats them specially.
struct CoreClasses {
    GlobalPtr<TypeClass> fromInt = nullptr;
    GlobalPtr<TypeClass> fromDecimal = nullptr;
    GlobalPtr<TypeClass> widen = nullptr;
    GlobalPtr<TypeClass> narrow = nullptr;
    GlobalPtr<TypeClass> truth = nullptr;

    // The three ownership classes. They are known by name for the same reason the five above are:
    // `let ->z = x` and the end of a value's lifetime are language syntax, and what they compile
    // to is a lookup of these. TrivialCopy and TrivialSink are deliberately *not* here - they are
    // structural facts with no instance to find, until Milestone 7 makes them constraints.
    GlobalPtr<TypeClass> copy = nullptr;
    GlobalPtr<TypeClass> sink = nullptr;
    GlobalPtr<TypeClass> drop = nullptr;
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

// The raw pointer type to `to`, interned per target type.
TypePtr resolvePointerType(Module& module, TypePtr to);

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

// Decides how a record is laid out, from the shape of its constructor list alone. This is
// deliberately independent of its type arguments: a generic body has to project into `Maybe(a)`
// the same way every instantiation does, so the declaration decides once and each instantiation
// inherits the answer.
void computeRecordLayout(GlobalBase base, RecordType& record);

bool finishTupleRepr(Module& module, TupType& tuple, LocationId source);
bool finishRecordRepr(Module& module, RecordType& record, LocationId source);

bool sameType(TypePtr lhs, TypePtr rhs);

// Whether two type argument lists are the same one. Interning makes this pointer equality per
// element, which is what instance selection, specialization caching and requirement matching all
// key on - so they all ask it here rather than each writing the loop. The second form compares a
// list where it is stored, without copying it out first.
bool sameTypes(Buffer<TypePtr> lhs, Buffer<TypePtr> rhs);

template<class List, class Base>
inline bool sameTypes(List& list, Base base, Buffer<TypePtr> args) {
    if(list.size() != args.length) return false;

    Size index = 0;
    for(auto type: list.contents(base)) {
        if(!sameType(type, args[index++])) return false;
    }

    return true;
}

/*
 * The ownership classification of a type, computed structurally and cached.
 *
 * `module` is needed only to find the authored instances, and the answer does not depend on which
 * module asked - see Type::ownership. A cycle reachable without an indirection would be an
 * infinitely large value and cannot be constructed; the guard exists so that a declaration which
 * *is* recursive (through a raw pointer, which is never recursed into) still terminates rather than
 * relying on the pointer case being reached first.
 */
Ownership ownershipOf(Module& module, TypePtr type);

// Whether the end of this value's lifetime has to run anything. Shorthand for the question drop
// insertion asks of every place, which is the one ownership fact most callers want.
bool needsDrop(Module& module, TypePtr type);

bool isUnit(GlobalBase base, TypePtr type);
bool isLiteral(GlobalBase base, TypePtr type);
bool isInteger(GlobalBase base, TypePtr type);
bool isPointer(GlobalBase base, TypePtr type);

// What a pointer points at, or null for anything else.
TypePtr pointeeType(GlobalBase base, TypePtr type);
bool isFloat(GlobalBase base, TypePtr type);
bool isNumeric(GlobalBase base, TypePtr type);
bool isGeneric(GlobalBase base, TypePtr type);
bool isDirectType(GlobalBase base, TypePtr type);
bool isMemoryType(GlobalBase base, TypePtr type);

U32 typeSize(GlobalBase base, TypePtr type);
U32 typeAlign(GlobalBase base, TypePtr type);

// How a type is written in a diagnostic or in printed IR. The builder form is the one that
// composes; the String form allocates a copy for a diagnostic argument.
void describeType(Context& context, GlobalBase base, TypePtr type, StringBuilder& target);
String describeType(Context& context, GlobalBase base, TypePtr type);

// A comma-separated list of types, as an argument list or an instance's types are written. Every
// diagnostic that names more than one type at once goes through this, so they all read alike.
void describeTypes(Context& context, GlobalBase base, Buffer<TypePtr> types, StringBuilder& target);
