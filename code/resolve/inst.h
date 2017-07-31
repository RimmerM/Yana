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
        InstZExt,
        InstSExt,

        InstAdd,
        InstSub,
        InstMul,
        InstIMul,
        InstDiv,
        InstIDiv,
        InstRem,
        InstCmp,
        InstICmp,

        InstShl,
        InstShr,
        InstSar,
        InstAnd,
        InstOr,
        InstXor,

        InstIf,
        InstBr,
        InstRet,
        InstPhi,
    };

    Block* block;
    Type* type;

    // Each instruction that uses this value.
    List<Use>* uses = nullptr;

    // Each block that uses this value.
    List<Block*> blockUses = nullptr;

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

struct InstBinary: Inst {
    Value* lhs, *rhs;
};

/*
 * Conversion instructions
 */
struct InstTrunc: Inst { Value* from; };
struct InstZExt: Inst { Value* from; };
struct InstSExt: Inst { Value* from; };

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

enum class Cmp {
    eq, neq, gt, ge, lt, le,
};

class InstCmp: InstBinary {
    Cmp cmp;
};

class InstICmp: InstCmp {};

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
struct InstIf: Inst {
    Value* cond;
    Block* then;
    Block* otherwise;
};

// Unconditional branch to a different block.
struct InstBr: Inst {
    Block* to;
};

// Return the provided value to the parent function.
struct InstRet: Inst {
    Value* value;
};

// ϕ-node, like LLVM. If any are used, they must be the first instructions in the block.
struct InstPhi: Inst {
    Block** incoming;
    Value** values;
    Size count;
};

inline bool isTerminating(Inst* inst) {
    return inst->kind == Inst::InstRet || inst->kind == Inst::InstIf || inst->kind == Inst::InstBr;
}
