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
    enum Kind {
        Arg,

        FirstConst,
        ConstInt = FirstConst,
        ConstFloat,
        ConstString,
        LastConst = ConstString,

        FirstInst,
        InstTrunc,
        InstFTrunc,
        InstZExt,
        InstSExt,
        InstFExt,

        InstAdd,
        InstSub,
        InstMul,
        InstIMul,
        InstDiv,
        InstIDiv,
        InstRem,
        InstFAdd,
        InstFSub,
        InstFMul,
        InstFDiv,

        InstCmp,
        InstICmp,
        InstFCmp,

        InstShl,
        InstShr,
        InstSar,
        InstAnd,
        InstOr,
        InstXor,

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
};

// A value provided through a function parameter.
struct Arg: Value {
    U32 index;
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

/*
 * Arithmetic instructions - these must be performed on two integers, float or vectors of the same type.
 */
struct InstAdd: InstBinary {};
struct InstSub: InstBinary {};
struct InstMul: InstBinary {};
struct InstIMul: InstBinary {};
struct InstDiv: InstBinary {};
struct InstIDiv: InstBinary {};
struct InstRem: InstBinary {};

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
    Value* value;
};

// ϕ-node, like LLVM. If any are used, they must be the first instructions in the block.
struct InstPhi: Inst {
    struct Alt {
        Block* fromBlock;
        Value* value;
    };

    // TODO: Find out the highest count that can be reached here in practice.
    using Alts = ArrayF<Alt, 4>;
    Alts alts;
};

inline bool isTerminating(Inst::Kind kind) {
    return kind == Inst::InstRet || kind == Inst::InstJe || kind == Inst::InstJmp;
}
