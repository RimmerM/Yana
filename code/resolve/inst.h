#pragma once

#include "../util/types.h"
#include "../compiler/context.h"

struct Value;
struct Inst;
struct Block;
struct Type;

// A single usage of a value by an instruction.
struct Use {
    Value* value;
    Inst* user;
};

// A local register containing the result of some operation.
struct Value {
    enum Kind: U16 {
        Arg,
        Global,

        FirstConst,
        ConstInt = FirstConst,
        ConstFloat,
        ConstString,
        LastConst = ConstString,

        FirstInst,

        // Primitives: conversion.
        InstTrunc = FirstInst,
        InstFTrunc,
        InstZExt,
        InstSExt,
        InstFExt,
        InstFToI,
        InstFToUI,
        InstIToF,
        InstUIToF,

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

        // Construction.
        InstRecord,
        InstTup,
        InstFun,

        // Memory.
        InstAlloc,
        InstLoad,
        InstLoadField,
        InstLoadGlobal,
        InstStore,
        InstStoreField,
        InstStoreGlobal,

        InstGetField,
        InstUpdateField,

        // Function calls.
        InstCall,
        InstCallGen,
        InstCallDyn,
        InstCallDynGen,
        InstCallForeign,

        // Control flow.
        InstJe,
        InstJmp,
        InstRet,
        InstPhi,
    };

    Block* block;
    Type* type;

    // Each instruction that uses this value.
    Array<Use> uses;

    // Each block that uses this value.
    Array<Block*> blockUses;

    // Data for use by the code generator.
    void* codegen = nullptr;

    U32 name;
    Kind kind;
    U16 id;
};

// A value provided through a function parameter.
struct Arg: Value {
    U32 index;
};

// A global value defined in a module.
struct Global: Value {
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
    I64 value;
};

struct ConstFloat: Value {
    double value;
};

struct ConstString: Value {
    const char* value;
    Size length;
};

// A single operation that can be performed inside a function block.
struct Inst: Value {
    Value** usedValues;
    Size usedCount = 0;
};

struct InstCast: Inst {
    Value* from;
};

struct InstBinary: Inst {
    Value* lhs, *rhs;
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
    Value* arg, *amount;
};

struct InstShl: InstShift {};
struct InstShr: InstShift {};
struct InstSar: InstShift {};

struct InstAnd: InstBinary {};
struct InstOr: InstBinary {};
struct InstXor: InstBinary {};

/*
 * Value construction.
 */
struct InstRecord: Inst {
    struct Con* con;
    Value** fields;
    Size fieldCount;
};

struct InstTup: Inst {
    Value** fields;
    Size fieldCount;
};

struct InstFun: Inst {
    struct Function* body;
    Value** frame;
    Size frameCount;
};

/*
 * Memory.
 */

// Allocates space for one instance of a type.
// The space is allocated on either the stack, GC heap or normal heap
// depending on the returned reference type and mutability.
struct InstAlloc: Inst {
    Type* valueType; // The amount of space to allocate.
    bool mut; // If disabled, the allocated value is guaranteed to not be modified after initialization.
};

// Loads a value from memory into a register.
// The value must be a reference type.
struct InstLoad: Inst {
    Value* from;
};

// Loads a single field from an aggregate type in memory into a register.
// The field to load is defined as a chain of field indices,
// allowing the loading from a contained field in a single operation.
struct InstLoadField: Inst {
    Value* from;
    U32* indexChain;
    U32 chainLength;
};

// Stores a value from a register into memory.
// The value stored into must be a reference type to the type stored.
struct InstStore: Inst {
    Value* to;
    Value* value;
};

// Stores a single field from a register into an aggregate type.
// The field to store into is defined as a chain of field indices,
// allowing storing into a contained field in a single operation.
struct InstStoreField: Inst {
    Value* to;
    Value* value;
    U32* indexChain;
    U32 chainLength;
};

// Takes a single field from an aggregate type in an existing register.
struct InstGetField: Inst {
    Value* from;
    U32* indexChain;
    U32 chainLength;
};

// Copies a register with an aggregate type while changing one or more fields.
struct InstUpdateField: Inst {
    struct Field {
        Value* value;
        U32 index;
    };

    Value* from;
    Field* fields;
    U32 fieldCount;
};

/*
 * Function calls.
 */
struct InstCall: Inst {
    struct Function* fun;
    Value** args;
    Size argCount;
};

struct InstCallGen: InstCall {};

struct InstCallDyn: Inst {
    Value* fun;
    Value** args;
    Size argCount;
};

struct InstCallDynGen: InstCallDyn {};

struct InstCallForeign: Inst {
    struct ForeignFunction* fun;
    Value** args;
    Size argCount;
};

/*
 * Control flow.
 */

// Conditional branch to one of two blocks.
struct InstJe: Inst {
    Value* cond;
    Block* then;
    Block* otherwise;
};

// Unconditional branch to a different block.
struct InstJmp: Inst {
    Block* to;
};

// Return the provided value to the parent function.
struct InstRet: Inst {
    // This can be null if the instruction returns nothing.
    Value* value;
};

// ϕ-node, like LLVM. If any are used, they must be the first instructions in the block.
struct InstPhi: Inst {
    struct Alt {
        Block* fromBlock;
        Value* value;
    };

    Alt* alts;
    Size altCount;
};

inline bool isTerminating(Inst::Kind kind) {
    return kind == Inst::InstRet || kind == Inst::InstJe || kind == Inst::InstJmp;
}

ConstInt* constInt(Block* block, I64 value);
ConstFloat* constFloat(Block* block, double value);
ConstString* constString(Block* block, const char* value, Size length);

InstTrunc* trunc(Block* block, Id name, Value* from, Type* to);
InstFTrunc* ftrunc(Block* block, Id name, Value* from, Type* to);
InstZExt* zext(Block* block, Id name, Value* from, Type* to);
InstSExt* sext(Block* block, Id name, Value* from, Type* to);
InstFExt* fext(Block* block, Id name, Value* from, Type* to);
InstIToF* itof(Block* block, Id name, Value* from, Type* to);
InstUIToF* uitof(Block* block, Id name, Value* from, Type* to);
InstFToI* ftoi(Block* block, Id name, Value* from, Type* to);
InstFToUI* ftoui(Block* block, Id name, Value* from, Type* to);

Value* add(Block* block, Id name, Value* lhs, Value* rhs);
Value* sub(Block* block, Id name, Value* lhs, Value* rhs);
Value* mul(Block* block, Id name, Value* lhs, Value* rhs);
Value* div(Block* block, Id name, Value* lhs, Value* rhs);
Value* idiv(Block* block, Id name, Value* lhs, Value* rhs);
Value* rem(Block* block, Id name, Value* lhs, Value* rhs);
Value* irem(Block* block, Id name, Value* lhs, Value* rhs);

Value* fadd(Block* block, Id name, Value* lhs, Value* rhs);
Value* fsub(Block* block, Id name, Value* lhs, Value* rhs);
Value* fmul(Block* block, Id name, Value* lhs, Value* rhs);
Value* fdiv(Block* block, Id name, Value* lhs, Value* rhs);

Value* icmp(Block* block, Id name, Value* lhs, Value* rhs, ICmp cmp);
Value* fcmp(Block* block, Id name, Value* lhs, Value* rhs, FCmp cmp);

Value* shl(Block* block, Id name, Value* arg, Value* amount);
Value* shr(Block* block, Id name, Value* arg, Value* amount);
Value* sar(Block* block, Id name, Value* arg, Value* amount);
Value* and_(Block* block, Id name, Value* lhs, Value* rhs);
Value* or_(Block* block, Id name, Value* lhs, Value* rhs);
Value* xor_(Block* block, Id name, Value* lhs, Value* rhs);

InstRecord* record(Block* block, Id name, struct Con* con, Value** fields, U32 count);
InstTup* tup(Block* block, Id name, Type* type, Value** fields, U32 count);
InstFun* fun(Block* block, Id name, struct Function* body, Type* type, Size frameCount);

InstAlloc* alloc(Block* block, Id name, Type* type, bool mut, bool local);
InstLoad* load(Block* block, Id name, Value* from);
InstLoadField* loadField(Block* block, Id name, Value* from, Type* type, U32* indices, U32 count);
InstStore* store(Block* block, Id name, Value* to, Value* value);
InstStoreField* storeField(Block* block, Id name, Value* to, Value* value, U32* indices, U32 count);

InstGetField* getField(Block* block, Id name, Value* from, Type* type, U32* indices, U32 count);
InstUpdateField* updateField(Block* block, Id name, Value* from, InstUpdateField::Field* fields, U32 count);

InstCall* call(Block* block, Id name, struct Function* fun, Value** args, U32 count);
InstCallGen* callGen(Block* block, Id name, struct Function* fun, Value** args, U32 count);
InstCallDyn* callDyn(Block* block, Id name, Value* fun, Value** args, U32 count);
InstCallDynGen* callDynGen(Block* block, Id name, Value* fun, Value** args, U32 count);
InstCallForeign* callForeign(Block* block, Id name, struct ForeignFunction* fun, Value** args, U32 count);

InstJe* je(Block* block, Value* cond, Block* then, Block* otherwise);
InstJmp* jmp(Block* block, Block* to);
InstRet* ret(Block* block, Value* value = nullptr);
InstPhi* phi(Block* block, Id name, InstPhi::Alt* alts, Size altCount);
