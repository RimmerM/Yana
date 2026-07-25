#pragma once

#include "../compiler/context.h"
#include "../util/container.h"

struct Value;
struct Inst;
struct Block;
struct Type;
struct Con;
struct Function;
struct Module;
struct GlobalRegion;

using GlobalBase = RegionBase<GlobalRegion>;
using TypePtr = RegionPtr<GlobalRegion, Type>;
using ConPtr = RegionPtr<GlobalRegion, Con>;

struct ModuleRegion;
using ModuleBase = RegionBase<ModuleRegion>;
template<class T> using ModulePtr = RegionPtr<ModuleRegion, T>;

// A local register containing the result of some operation.
struct Value {
    enum Kind: U16 {
        Arg,
        Global,

        FirstConst,
        ConstInt = FirstConst,
        ConstFloat,
        ConstDouble,
        ConstString,
        LastConst = ConstString,

        FirstInst,
        InstNop = FirstInst,

        // Primitives: conversion.
        FirstCast,
        InstTrunc = FirstCast,
        InstFTrunc,
        InstZExt,
        InstSExt,
        InstFExt,
        InstFToI,
        InstFToUI,
        InstIToF,
        InstUIToF,
        LastCast = InstUIToF,

        // Primitives: arithmetic.
        InstAdd,
        InstSub,
        InstMul,
        InstDiv,
        InstIDiv,
        InstRem,
        InstIRem,
        InstFAdd,
        InstFSub,
        InstFMul,
        InstFDiv,

        InstICmp,
        InstFCmp,

        InstShl,
        InstShr,
        InstSar,
        InstAnd,
        InstOr,
        InstXor,

        // Pointer arithmetic.
        InstAddPtr,

        // Construction.
        InstRecord,
        InstTup,
        InstFun,

        // Memory.
        InstAlloc,
        InstAllocArray,
        InstLoad,
        InstLoadField,
        InstLoadArray,
        InstStore,
        InstStoreField,
        InstStoreArray,

        InstGetField,
        InstUpdateField,

        // Arrays.
        InstArrayLength,
        InstArrayCopy,
        InstArraySlice,

        // Strings.
        InstStringLength,
        InstStringData,

        // Function calls.
        FirstCall,
        InstCall = FirstCall,
        InstCallDyn,
        InstCallForeign,
        LastCall = InstCallForeign,

        // Control flow.
        FirstTerminating,
        InstJe = FirstTerminating,
        InstJmp,
        InstRet,
        LastTerminating = InstRet,

        InstPhi,
    };

    // Opaque data for use by the code generator.
    void* codegen = nullptr;

    ModulePtr<Block> block;
    TypePtr type;
    SmallList<ModuleRegion, ModulePtr<Inst>, false> uses;

    LocationId source = kNullLocation;
    StringId name = 0;
    Kind kind;
    U16 id = 0;
    U16 usedCount = 0;
    U16 usedOffset = 0;

    Value(Kind kind, ModulePtr<Block> block, TypePtr type): block(block), type(type), kind(kind) {}

    Buffer<ModulePtr<Value>> used() {
        auto used = (ModulePtr<Value>*)(((Byte*)this) + usedOffset * 4);
        return { used, usedCount };
    }
};

// A value provided through a function parameter.
struct Arg: Value {
    using Value::Value;
    U32 index;
};

// A global value defined in a module.
struct Global: Value {
    using Value::Value;

    // Used for lazy resolving of AST nodes.
    // Set until the global is fully resolved.
    void* ast = nullptr;

    // Globals and functions can be interdependent.
    // This is no problem in most cases, except when their inferred types depend on each other,
    // which could cause infinite recursion.
    // We use this flag to detect that condition and throw an error.
    bool resolving = false;
};

// An immediate value that can be used by instructions.
struct ConstInt: Value {
    ConstInt(ModulePtr<Block> block, TypePtr type): Value(Value::ConstInt, block, type) {}
    U64 value;
};

struct ConstFloat: Value {
    ConstFloat(ModulePtr<Block> block, TypePtr type): Value(Value::ConstFloat, block, type) {}
    float value;
};

struct ConstDouble: Value {
    ConstDouble(ModulePtr<Block> block, TypePtr type): Value(Value::ConstDouble, block, type) {}
    double value;
};

struct ConstString: Value {
    ConstString(ModulePtr<Block> block, TypePtr type): Value(Value::ConstString, block, type) {}
    StringId value;
};

// A single operation that can be performed inside a function block.
struct Inst: Value {
    using Value::Value;
};

struct InstCast: Inst {
    ModulePtr<Value> from;
};

struct InstBinary: Inst {
    ModulePtr<Value> lhs, rhs;
};

/*
 * Conversion instructions
 */
struct InstTrunc: InstCast {};
struct InstFTrunc: InstCast {};
struct InstZExt: InstCast {};
struct InstSExt: InstCast {};
struct InstFExt: InstCast {};
struct InstFToI: InstCast {};
struct InstFToUI: InstCast {};
struct InstIToF: InstCast {};
struct InstUIToF: InstCast {};

/*
 * Arithmetic instructions - these must be performed on two integers, float or vectors of the same type.
 */
struct InstAdd: InstBinary {};
struct InstSub: InstBinary {};
struct InstMul: InstBinary {};
struct InstDiv: InstBinary {};
struct InstIDiv: InstBinary {};
struct InstRem: InstBinary {};
struct InstIRem: InstBinary {};

struct InstFAdd: InstBinary {};
struct InstFSub: InstBinary {};
struct InstFMul: InstBinary {};
struct InstFDiv: InstBinary {};

enum class ICmp {
    eq, neq, gt, ge, lt, le, igt, ige, ilt, ile,
};

struct InstICmp: InstBinary {
    ICmp cmp;
};

enum class FCmp {
    eq, neq, gt, ge, lt, le,
};

struct InstFCmp: InstBinary {
    FCmp cmp;
};

/*
 * Bitwise instructions - must be performed on integer types or integer vectors
 */
struct InstShift: Inst {
    ModulePtr<Value> arg, amount;
};

struct InstShl: InstShift {};
struct InstShr: InstShift {};
struct InstSar: InstShift {};

struct InstAnd: InstBinary {};
struct InstOr: InstBinary {};
struct InstXor: InstBinary {};

/*
 * Reference instructions - must be performed on untraced reference types.
 */
struct InstAddPtr: InstBinary {};

/*
 * Value construction.
 */
struct InstRecord: Inst {
    ConPtr con;
    ModulePtr<Value> content;
};

struct InstTup: Inst {
    // Fields are stored in the used values list.
};

struct InstFun: Inst {
    // Function must be defined in the same module.
    ModulePtr<Function> body;
};

/*
 * Memory.
 */

// Allocates space for one instance of a type.
// The space is allocated on either the stack, GC heap or normal heap
// depending on the returned reference type and mutability.
struct InstAlloc: Inst {
    TypePtr valueType; // The amount of space to allocate.
    bool heap; // If set, the allocation is put on the heap. Otherwise, on the stack.
    bool mut; // If disabled, the allocated value is guaranteed to not be modified after initialization.
};

// Allocates space for an array of instances of a type.
// The space is allocated on either the stack, GC heap or normal heap
// depending on the returned reference type and mutability.
struct InstAllocArray: Inst {
    TypePtr valueType; // The amount of space to allocate for each array slot.
    ModulePtr<Value> length; // The number of slots to allocate.
    bool heap; // If set, the allocation is put on the heap. Otherwise, on the stack.
    bool mut; // If disabled, the allocated value is guaranteed to not be modified after initialization.
};

// Loads a value from memory into a register.
// The value must be a reference type.
struct InstLoad: Inst {
    ModulePtr<Value> from;
};

// Loads a single field from an aggregate type in memory into a register.
// The field to load is defined as a chain of field indices,
// allowing the loading from a contained field in a single operation.
// The field chain works as follows:
// forEach(chain) {#element, #index}:
//   if element is Record:
//     if index == 0:
//       get element constructor index
//     else:
//       cast element to element.cons.(index - 1)
//     continue
//   if element is Tuple:
//     element.getfield(index)
//     continue
struct InstLoadField: Inst {
    ModulePtr<Value> from;
    SmallList<ModuleRegion, U16> indexChain;
};

// Loads a single field from an array in memory into a register.
// If the load is checked, the runtime fails if the index is out of bounds.
struct InstLoadArray: Inst {
    ModulePtr<Value> from;
    ModulePtr<Value> index;
    bool checked;
};

// Stores a value from a register into memory.
// The value stored into must be a reference type to the type stored.
struct InstStore: Inst {
    ModulePtr<Value> to;
    ModulePtr<Value> value;
};

// Stores a single field from a register into an aggregate type.
// The field to store into is defined as a chain of field indices,
// allowing storing into a contained field in a single operation.
// The chain works the same as for InstLoadField, but stores instead.
struct InstStoreField: Inst {
    ModulePtr<Value> to;
    ModulePtr<Value> value;
    SmallList<ModuleRegion, U16> indexChain;
};

// Stores a single field from a register into an array.
// If the store is checked, the runtime fails if the index is out of bounds.
struct InstStoreArray: Inst {
    U32 count: 31;
    bool checked: 1;

    ModulePtr<Value> to;
    ModulePtr<Value> index;
    ModulePtr<Value> values[];
};

// Takes a single field from an aggregate type in an existing register.
// The fields work the same way as for InstLoadField.
struct InstGetField: Inst {
    ModulePtr<Value> from;
    SmallList<ModuleRegion, U16> indexChain;
};

// Copies a register with an aggregate type while changing one or more fields.
struct InstUpdateField: Inst {
    SmallList<ModuleRegion, U16> fieldIndexes;
    ModulePtr<Value> from;
    ModulePtr<Value> fieldValues[];
};

/*
 * Arrays.
 */

// Returns the number of items an array currently contains.
struct InstArrayLength: Inst {
    ModulePtr<Value> from;
};

// Copies elements from one array to another.
// The arrays must have the same type.
// If the copy is checked, the runtime fails if the index is out of bounds.
struct InstArrayCopy: Inst {
    ModulePtr<Value> from;
    ModulePtr<Value> to;
    ModulePtr<Value> startIndex;
    ModulePtr<Value> count;
    bool checked;
};

// Creates an array representing a slice into an existing array, without copying if possible.
// Slices are always represented as an immutable reference to an array.
struct InstArraySlice: Inst {
    ModulePtr<Value> from;
    ModulePtr<Value> startIndex;
    ModulePtr<Value> count;
};

/*
 * Strings.
 */

// Returns the string length as an integer.
struct InstStringLength: Inst {
    ModulePtr<Value> from;
};

// Returns platform-specific a string data.
// On native platforms, this returns a pointer to the actual string bytes.
// On JS platforms, this returns a native value containing a string.
struct InstStringData: Inst {
    ModulePtr<Value> from;
};

/*
 * Function calls.
 */
struct InstCall: Inst {
    RegionPtr<GlobalRegion, Function> fun;
    ModulePtr<Value> args[];
};

struct InstCallDyn: Inst {
    // If this is set, the function call should be interpreted as an intrinsic.
    // This can mean multiple things:
    //  - For the native target:
    //     - Calling an IntType value will generate a system call for that value.
    //     - Calling a StringType value will generate an llvm intrinsic call.
    //  - For the JS target:
    //     - Calling a StringType value will generate a call to a native JS function.
    bool isIntrinsic;

    ModulePtr<Value> fun;

    // argCount = Value::usedCount - 1.
    ModulePtr<Value> args[];
};

struct InstCallForeign: Inst {
    RegionPtr<GlobalRegion, struct ForeignFunction> fun;
    ModulePtr<Value> args[];
};

/*
 * Control flow.
 */

// Conditional branch to one of two blocks.
struct InstJe: Inst {
    ModulePtr<Value> cond;
    ModulePtr<Block> then;
    ModulePtr<Block> otherwise;
};

// Unconditional branch to a different block.
struct InstJmp: Inst {
    ModulePtr<Block> to;
};

// Return the provided value to the parent function.
struct InstRet: Inst {
    // This can be null if the instruction returns nothing.
    ModulePtr<Value> value;
};

// ϕ-node, like LLVM. If any are used, they must be the first instructions in the block.
struct InstPhi: Inst {
    SmallList<ModuleRegion, ModulePtr<Block>, false> sourceBlocks;
    ModulePtr<Value> sourceValues[];
};

inline bool isLiteral(Inst::Kind inst) {
    return inst >= Value::FirstConst && inst <= Value::LastConst;
}

inline bool isCast(Inst::Kind inst) {
    return inst >= Value::FirstCast && inst <= Value::LastCast;
}

inline bool isTerminator(Inst::Kind inst) {
    return inst >= Value::FirstTerminating && inst <= Value::LastTerminating;
}

inline bool isCall(Inst::Kind inst) {
    return inst >= Value::FirstCall && inst <= Value::LastCall;
}

inline bool isPhi(Inst::Kind inst) {
    return inst == Value::InstPhi;
}

Value* error(Module* module, Block* block, StringId name, Type* type);
Value* nop(Module* module, Block* block, StringId name);

ConstInt* constInt(Module* module, Block* block, StringId name, U64 value, Type* type);
ConstFloat* constFloat(Module* module, Block* block, StringId name, float value, Type* type);
ConstDouble* constDouble(Module* module, Block* block, StringId name, double value, Type* type);
ConstString* constString(Module* module, Block* block, StringId name, const char* value, Size length);

Value* trunc(Module* module, Block* block, StringId name, Value* from, Type* to);
Value* ftrunc(Module* module, Block* block, StringId name, Value* from, Type* to);
Value* zext(Module* module, Block* block, StringId name, Value* from, Type* to);
Value* sext(Module* module, Block* block, StringId name, Value* from, Type* to);
Value* fext(Module* module, Block* block, StringId name, Value* from, Type* to);
Value* itof(Module* module, Block* block, StringId name, Value* from, Type* to);
Value* uitof(Module* module, Block* block, StringId name, Value* from, Type* to);
Value* ftoi(Module* module, Block* block, StringId name, Value* from, Type* to);
Value* ftoui(Module* module, Block* block, StringId name, Value* from, Type* to);

Value* add(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* sub(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* mul(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* div(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* idiv(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* rem(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* irem(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);

Value* fadd(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* fsub(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* fmul(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* fdiv(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);

Value* icmp(Module* module, Block* block, StringId name, Value* lhs, Value* rhs, ICmp cmp);
Value* fcmp(Module* module, Block* block, StringId name, Value* lhs, Value* rhs, FCmp cmp);

Value* shl(Module* module, Block* block, StringId name, Value* arg, Value* amount);
Value* shr(Module* module, Block* block, StringId name, Value* arg, Value* amount);
Value* sar(Module* module, Block* block, StringId name, Value* arg, Value* amount);
Value* and_(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* or_(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);
Value* xor_(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);

Value* addptr(Module* module, Block* block, StringId name, Value* lhs, Value* rhs);

InstRecord* record(Module* module, Block* block, StringId name, struct Con* con, Value* content);
InstTup* tup(Module* module, Block* block, StringId name, Type* type, Value** fields, U32 count);
InstFun* fun(Module* module, Block* block, StringId name, Function* body, Type* type, Size frameCount);

InstAlloc* alloc(Module* module, Block* block, StringId name, Type* type, bool mut, bool local);
InstAllocArray* allocArray(Module* module, Block* block, StringId name, Type* type, Value* length, bool mut, bool local);

InstLoad* load(Module* module, Block* block, StringId name, Value* from);
InstLoadField* loadField(Module* module, Block* block, StringId name, Value* from, Type* type, U32* indices, U32 count);
InstLoadArray* loadArray(Module* module, Block* block, StringId name, Value* from, Value* index, Type* type, bool checked);

InstStore* store(Module* module, Block* block, StringId name, Value* to, Value* value);
InstStoreField* storeField(Module* module, Block* block, StringId name, Value* to, Value* value, U32* indices, U32 count);
InstStoreArray* storeArray(Module* module, Block* block, StringId name, Value* to, Value* index, Value** values, U32 count, bool checked);

InstGetField* getField(Module* module, Block* block, StringId name, Value* from, Type* type, U32* indices, U32 count);
InstUpdateField* updateField(Module* module, Block* block, StringId name, Value* from, InstUpdateField::Field* fields, U32 count);

InstArrayLength* arrayLength(Module* module, Block* block, StringId name, Value* from);
InstArrayCopy* arrayCopy(Module* module, Block* block, StringId name, Value* from, Value* to, Value* offset, Value* count, bool checked);
InstArraySlice* arraySlice(Module* module, Block* block, StringId name, Value* from, Value* start, Value* count);

Value* stringLength(Module* module, Block* block, StringId name, Value* from);
Value* stringData(Module* module, Block* block, StringId name, Value* from);

InstCall* call(Module* module, Block* block, StringId name, Function* fun, Value** args, U32 count);
InstCallDyn* callDyn(Module* module, Block* block, StringId name, Value* fun, Type* type, Value** args, U32 count, bool isIntrinsic);
InstCallForeign* callForeign(Module* module, Block* block, StringId name, struct ForeignFunction* fun, Value** args, U32 count);

InstJe* je(Module* module, Block* block, Value* cond, Block* then, Block* otherwise);
InstJmp* jmp(Module* module, Block* block, Block* to);
InstRet* ret(Module* module, Block* block, Value* value = nullptr);
InstPhi* phi(Module* module, Block* block, StringId name, Size altCount);
