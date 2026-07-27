#pragma once

#include "type.h"

struct Block;
struct Function;
struct ModuleRegion {};

using ModuleBase = RegionBase<ModuleRegion>;

template<class T>
using ModulePtr = RegionPtr<ModuleRegion, T>;

template<class T, bool allowEmbed = true>
using ModuleList = SmallList<ModuleRegion, T, allowEmbed>;

struct Inst;
struct Value;
struct Global;

enum class ProjectionKind: U8 {
    Discriminant,
    Field,
    Deref,
    Index,
    Downcast,
};

struct Projection {
    ProjectionKind kind;
    U16 index;
    ModulePtr<Value> value = nullptr;
};

/*
 * What a place is rooted in.
 *
 * Implementation-IR.md part 2 states a place as a local plus a path into it, which is all the
 * ownership model ever needs: a local is storage whose lifetime the function knows. Raw memory
 * adds the two roots whose lifetime it does not - a module-level global, and an address the
 * program computed for itself.
 *
 * The distinction is the one the borrow checker will care about. A local root can be proved
 * things about; a pointer root cannot be proved anything about at all, which is exactly what
 * Design.md means by the Native module being unsafe.
 */
enum class PlaceRoot: U8 {
    Local,
    Global,
    Pointer,
};

struct Place {
    static Place inLocal(U32 local) { return Place { PlaceRoot::Local, local }; }
    static Place inGlobal(ModulePtr<Global> global) { return Place { PlaceRoot::Global, 0, global }; }

    // The memory a raw pointer names. The address is the root itself rather than something loaded
    // from it, so `*p` is one place with no projections rather than a place plus a Deref.
    static Place atPointer(ModulePtr<Value> pointer) {
        return Place { PlaceRoot::Pointer, 0, nullptr, pointer };
    }

    PlaceRoot root = PlaceRoot::Local;
    U32 local = 0;
    ModulePtr<Global> global = nullptr;
    ModulePtr<Value> pointer = nullptr;
    ModuleList<Projection, false> projections;
};

struct Value {
    enum Kind: U8 {
        Arg,
        ConstInt,
        ConstFloat,
        ConstDouble,
        Alloc,
        LoadPlace,
        Init,
        Address,
        Native,
        Cast,
        Neg,
        Not,
        Add,
        Sub,
        Mul,
        Div,
        Rem,
        Shl,
        Shr,
        Sar,
        And,
        Or,
        Xor,
        Cmp,
        Call,
        GenCall,
        Je,
        Jmp,
        Ret,
        Phi,
    };

    Value(Kind kind, ModulePtr<Block> block, TypePtr type):
        block(block), type(type), kind(kind) {}

    ModulePtr<Block> block;
    TypePtr type;
    ModuleList<ModulePtr<Inst>, false> uses;
    LocationId source = kNullLocation;
    StringId name = 0;
    U32 id = 0;
    Kind kind;
};

struct Arg: Value {
    Arg(ModulePtr<Block> block, TypePtr type, U16 index):
        Value(Value::Arg, block, type), index(index) {}

    U16 index;
};

struct ConstInt: Value {
    ConstInt(ModulePtr<Block> block, TypePtr type, U64 value):
        Value(Value::ConstInt, block, type), value(value) {}

    U64 value;
};

struct ConstFloat: Value {
    ConstFloat(ModulePtr<Block> block, TypePtr type, F32 value):
        Value(Value::ConstFloat, block, type), value(value) {}

    F32 value;
};

struct ConstDouble: Value {
    ConstDouble(ModulePtr<Block> block, TypePtr type, F64 value):
        Value(Value::ConstDouble, block, type), value(value) {}

    F64 value;
};

// Every instruction takes its block and its result type as its first two constructor arguments,
// followed by whatever it is made of. That order is what lets builder.h construct any of them
// through one function instead of one overload per kind.
struct Inst: Value {
    using Value::Value;
};

struct InstAlloc: Inst {
    InstAlloc(ModulePtr<Block> block, TypePtr type, U32 local):
        Inst(Value::Alloc, block, type), local(local) {}

    U32 local;
};

struct InstLoadPlace: Inst {
    InstLoadPlace(ModulePtr<Block> block, TypePtr type, Place place):
        Inst(Value::LoadPlace, block, type), place(place) {}

    Place place;
};

struct InstInit: Inst {
    InstInit(ModulePtr<Block> block, TypePtr unit, Place place, ModulePtr<Value> value):
        Inst(Value::Init, block, unit), place(place), value(value) {}

    Place place;
    ModulePtr<Value> value;
};

// The address of a place, as a raw pointer. This is what `addressOf` compiles to, and it is the
// one operation that forces storage to exist: a value it is applied to cannot stay in a register,
// which is the "writable, stable-address representation requirement" Design.md's Pointers section
// gives to anything a raw pointer is taken of.
struct InstAddress: Inst {
    InstAddress(ModulePtr<Block> block, TypePtr type, Place place):
        Inst(Value::Address, block, type), place(place) {}

    Place place;
};

/*
 * An operation of the Native module that is not expressible as anything more basic.
 *
 * These are one instruction rather than one kind each because they have nothing in common with
 * the rest of the IR and everything in common with each other: a fixed operation, a flat argument
 * list, and a meaning that is the machine's rather than the language's. Everything the resolver
 * does with them is the same for all of them, so a kind per operation would be a case per
 * operation in five switches to say the same thing five times.
 */
enum class NativeOp: U8 {
    // copyMemory(to, from, count) and setMemory(to, value, count) - the two block operations the
    // lower IR already has, reached by name instead of derived from an aggregate assignment.
    CopyMemory,
    SetMemory,

    // syscall(number, args...). The lower IR and the x64 backend already model this as a call with
    // its own convention, so the number is operand zero here exactly as it is there.
    Syscall,
};

struct InstNative: Inst {
    InstNative(ModulePtr<Block> block, TypePtr type, NativeOp op):
        Inst(Value::Native, block, type), op(op) {}

    ModuleList<ModulePtr<Value>, false> args;
    NativeOp op;
};

struct InstUnary: Inst {
    InstUnary(ModulePtr<Block> block, TypePtr type, Kind kind, ModulePtr<Value> from):
        Inst(kind, block, type), from(from) {}

    ModulePtr<Value> from;
};

struct InstBinary: Inst {
    InstBinary(ModulePtr<Block> block, TypePtr type, Kind kind, ModulePtr<Value> lhs, ModulePtr<Value> rhs):
        Inst(kind, block, type), lhs(lhs), rhs(rhs) {}

    ModulePtr<Value> lhs;
    ModulePtr<Value> rhs;
};

enum class CompareOp: U8 {
    Eq,
    Ne,
    Gt,
    Ge,
    Lt,
    Le,
};

struct InstCmp: InstBinary {
    InstCmp(ModulePtr<Block> block, TypePtr type, ModulePtr<Value> lhs, ModulePtr<Value> rhs, CompareOp cmp):
        InstBinary(block, type, Value::Cmp, lhs, rhs), cmp(cmp) {}

    CompareOp cmp;
};

struct InstCall: Inst {
    InstCall(ModulePtr<Block> block, TypePtr type, ModulePtr<Function> callee):
        Inst(Value::Call, block, type), callee(callee) {}

    ModulePtr<Function> callee;
    ModuleList<ModulePtr<Value>, false> args;
    U32 local = maxLimit<U32>;
};

/*
 * A call whose callee is not decided yet, because deciding it needs types this function does not
 * have. It appears only inside a generic body and never survives specialization: cloning
 * substitutes `typeArgs`, and a concrete argument list turns the instruction into an ordinary
 * InstCall to the class implementation or to the callee's specialization.
 *
 * Keeping the resolved decision here rather than re-deriving it later is what Implementation-
 * Generics.md's first invariant asks for. The body has already decided *which class function*
 * and *with which type arguments*; specialization only supplies the instance.
 */
struct InstGenCall: Inst {
    InstGenCall(ModulePtr<Block> block, TypePtr type, ModulePtr<Function> callee,
                GlobalPtr<TypeClass> typeClass, U16 index):
        Inst(Value::GenCall, block, type), callee(callee), typeClass(typeClass), index(index) {}

    // The class signature this dispatches to, or the generic function being called.
    ModulePtr<Function> callee;

    // Set for a class dispatch, with `index` naming the function within the class. Null when the
    // callee is a generic function, whose `typeArgs` are its own context's instead.
    GlobalPtr<TypeClass> typeClass;

    ModuleList<TypePtr, false> typeArgs;
    ModuleList<ModulePtr<Value>, false> args;
    U16 index;
    U32 local = maxLimit<U32>;
};

struct InstJe: Inst {
    InstJe(ModulePtr<Block> block, TypePtr unit, ModulePtr<Value> cond,
           ModulePtr<Block> thenBlock, ModulePtr<Block> elseBlock):
        Inst(Value::Je, block, unit), cond(cond), thenBlock(thenBlock), elseBlock(elseBlock) {}

    ModulePtr<Value> cond;
    ModulePtr<Block> thenBlock;
    ModulePtr<Block> elseBlock;
};

struct InstJmp: Inst {
    InstJmp(ModulePtr<Block> block, TypePtr unit, ModulePtr<Block> target):
        Inst(Value::Jmp, block, unit), target(target) {}

    ModulePtr<Block> target;
};

struct InstRet: Inst {
    InstRet(ModulePtr<Block> block, TypePtr unit, ModulePtr<Value> value):
        Inst(Value::Ret, block, unit), value(value) {}

    ModulePtr<Value> value;
};

struct PhiInput {
    ModulePtr<Block> block;
    ModulePtr<Value> value;
};

struct InstPhi: Inst {
    InstPhi(ModulePtr<Block> block, TypePtr type): Inst(Value::Phi, block, type) {}

    ModuleList<PhiInput, false> inputs;
};

bool isTerminator(const Value& value);
bool isConstant(const Value& value);
