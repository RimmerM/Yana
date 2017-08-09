#include <csetjmp>
#include "module.h"

enum Exception {
    Exit = 1,
    StackOverflow,
    NoMemory,
    InvalidOp,
};

struct Eval {
    U64* sp;
    U64* bp;
    U64* ap;
    Inst** pc;
    Block* lb;

    Arena heap;
    U32 heapSize;
    U32 heapMax;

    U64* sb;
    jmp_buf* raise;
};

template<class T>
U64 asReg(T v) {
    return *(U64*)&v;
}

void push8(Eval* eval, U8 v) {
    *((U8*)eval->sp) = v;
    eval->sp++;
}

void push16(Eval* eval, U16 v) {
    *((U16*)eval->sp) = v;
    eval->sp++;
}

void push32(Eval* eval, U32 v) {
    *((U32*)eval->sp) = v;
    eval->sp++;
}

void push64(Eval* eval, U64 v) {
    *((U64*)eval->sp) = v;
    eval->sp++;
}

void pushp(Eval* eval, void* p) {
    *((void**)eval->sp) = p;
    eval->sp++;
}

U8 pop8(Eval* eval) {
    U8 p = *((U8*)eval->sp);
    eval->sp--;
    return p;
}

U16 pop16(Eval* eval) {
    U16 p = *((U16*)eval->sp);
    eval->sp--;
    return p;
}

U32 pop32(Eval* eval) {
    U32 p = *((U32*)eval->sp);
    eval->sp--;
    return p;
}

U64 pop64(Eval* eval) {
    U64 p = *((U64*)eval->sp);
    eval->sp--;
    return p;
}

U64* popp(Eval* eval) {
    U64* p = *((U64**)eval->sp);
    eval->sp--;
    return p;
}

void setBlock(Eval* eval, Block* block) {
    eval->pc = block->instructions.pointer();
}

void funIntro(Eval* eval, Function* fun) {
    pushp(eval, eval->pc);
    pushp(eval, eval->bp);
    pushp(eval, eval->ap);
    eval->bp = eval->sp;
    eval->ap = eval->sp - (3 + fun->args.size());
    eval->sp += fun->instCounter;
    setBlock(eval, fun->blocks[0]);
}

void raise(Eval* eval, Exception exception) {
    longjmp(*eval->raise, exception);
}

U64 evalInt(Eval* eval, Value* v) {
    auto kind = v->kind;
    if(kind == Value::Arg) {
        auto arg = (Arg*)v;
        return *(U64*)(eval->ap + arg->index);
    } else if(kind >= Value::FirstInst) {
        return *(U64*)(eval->bp + v->id);
    } else if(kind == Value::ConstInt) {
        auto i = (ConstInt*)v;
        return i->value;
    } else {
        // Unsupported.
    }
}

double evalFloat(Eval* eval, Value* v) {
    auto kind = v->kind;
    if(kind == Value::Arg) {
        auto arg = (Arg*)v;
        return *(double*)(eval->ap + arg->index);
    } else if(kind >= Value::FirstInst) {
        return *(double*)(eval->bp + v->id);
    } else if(kind == Value::ConstFloat) {
        auto f = (ConstFloat*)v;
        return f->value;
    } else {
        // Unsupported.
    }
}

U64 evalValue(Eval* eval, Value* v) {
    auto kind = v->kind;
    if(kind == Value::Arg) {
        auto arg = (Arg*)v;
        return eval->ap[arg->index];
    } else if(kind >= Value::FirstInst) {
        return eval->bp[v->id];
    } else if(kind == Value::ConstInt) {
        auto i = (ConstInt*)v;
        return asReg(i->value);
    } else if(kind == Value::ConstFloat) {
        auto f = (ConstFloat*)v;
        return asReg(f->value);
    } else {
        // Unsupported.
    }
}

U64 evalNop(Eval* eval, Inst* inst) {
    return 0;
}

U64 evalTrunc(Eval* eval, InstTrunc* inst) {
    auto i = evalInt(eval, inst->from);
    switch(((IntType*)inst->type)->width) {
        case IntType::Bool:
            return (U64)(i != 0);
        case IntType::Int:
            return (U32)i;
        default:
            return i;
    }
}

U64 evalFTrunc(Eval* eval, InstFTrunc* inst) {
    return asReg(evalFloat(eval, inst->from));
}

U64 evalZExt(Eval* eval, InstZExt* inst) {
    return evalInt(eval, inst->from);
}

U64 evalSExt(Eval* eval, InstSExt* inst) {
    return evalInt(eval, inst->from);
}

U64 evalFExt(Eval* eval, InstFExt* inst) {
    return asReg(evalFloat(eval, inst->from));
}

U64 evalFToI(Eval* eval, InstFToI* inst) {
    return (U64)((I64)evalFloat(eval, inst->from));
}

U64 evalFToUI(Eval* eval, InstFToI* inst) {
    return (U64)evalFloat(eval, inst->from);
}

U64 evalIToF(Eval* eval, InstIToF* inst) {
    return asReg((double)((I64)evalInt(eval, inst->from)));
}

U64 evalUIToF(Eval* eval, InstUIToF* inst) {
    return asReg((double)evalInt(eval, inst->from));
}

U64 evalAdd(Eval* eval, InstAdd* inst) {
    return evalInt(eval, inst->lhs) + evalInt(eval, inst->rhs);
}

U64 evalSub(Eval* eval, InstSub* inst) {
    return evalInt(eval, inst->lhs) - evalInt(eval, inst->rhs);
}

U64 evalMul(Eval* eval, InstMul* inst) {
    return evalInt(eval, inst->lhs) * evalInt(eval, inst->rhs);
}

U64 evalDiv(Eval* eval, InstDiv* inst) {
    return evalInt(eval, inst->lhs) / evalInt(eval, inst->rhs);
}

U64 evalIDiv(Eval* eval, InstIDiv* inst) {
    return (U64)((I64)evalInt(eval, inst->lhs) / (I64)evalInt(eval, inst->rhs));
}

U64 evalRem(Eval* eval, InstRem* inst) {
    return evalInt(eval, inst->lhs) % evalInt(eval, inst->rhs);
}

U64 evalIRem(Eval* eval, InstIRem* inst) {
    return (U64)((I64)evalInt(eval, inst->lhs) % (I64)evalInt(eval, inst->rhs));
}

U64 evalFAdd(Eval* eval, InstFAdd* inst) {
    return asReg(evalFloat(eval, inst->lhs) + evalFloat(eval, inst->rhs));
}

U64 evalFSub(Eval* eval, InstFSub* inst) {
    return asReg(evalFloat(eval, inst->lhs) - evalFloat(eval, inst->rhs));
}

U64 evalFMul(Eval* eval, InstFMul* inst) {
    return asReg(evalFloat(eval, inst->lhs) * evalFloat(eval, inst->rhs));
}

U64 evalFDiv(Eval* eval, InstFDiv* inst) {
    return asReg(evalFloat(eval, inst->lhs) / evalFloat(eval, inst->rhs));
}

U64 evalICmp(Eval* eval, InstICmp* inst) {
    auto lhs = evalInt(eval, inst->lhs);
    auto rhs = evalInt(eval, inst->rhs);
    switch(inst->cmp) {
        case ICmp::eq:
            return (U64)(lhs == rhs);
        case ICmp::neq:
            return (U64)(lhs != rhs);
        case ICmp::gt:
            return (U64)(lhs > rhs);
        case ICmp::ge:
            return (U64)(lhs >= rhs);
        case ICmp::lt:
            return (U64)(lhs < rhs);
        case ICmp::le:
            return (U64)(lhs <= rhs);
        case ICmp::igt:
            return (U64)((I64)lhs > (I64)rhs);
        case ICmp::ige:
            return (U64)((I64)lhs >= (I64)rhs);
        case ICmp::ilt:
            return (U64)((I64)lhs < (I64)rhs);
        case ICmp::ile:
            return (U64)((I64)lhs <= (I64)rhs);
    }
}

U64 evalFCmp(Eval* eval, InstFCmp* inst) {
    auto lhs = evalFloat(eval, inst->lhs);
    auto rhs = evalFloat(eval, inst->rhs);
    switch(inst->cmp) {
        case FCmp::eq:
            return (U64)(lhs == rhs);
        case FCmp::neq:
            return (U64)(lhs != rhs);
        case FCmp::gt:
            return (U64)(lhs > rhs);
        case FCmp::ge:
            return (U64)(lhs >= rhs);
        case FCmp::lt:
            return (U64)(lhs < rhs);
        case FCmp::le:
            return (U64)(lhs <= rhs);
    }
}

U64 evalShl(Eval* eval, InstShl* inst) {
    return evalInt(eval, inst->arg) << evalInt(eval, inst->amount);
}

U64 evalShr(Eval* eval, InstShr* inst) {
    return evalInt(eval, inst->arg) >> evalInt(eval, inst->amount);
}

U64 evalSar(Eval* eval, InstSar* inst) {
    return (U64)((I64)evalInt(eval, inst->arg) >> (I64)evalInt(eval, inst->amount));
}

U64 evalAnd(Eval* eval, InstAnd* inst) {
    return evalInt(eval, inst->lhs) & evalInt(eval, inst->rhs);
}

U64 evalOr(Eval* eval, InstOr* inst) {
    return evalInt(eval, inst->lhs) | evalInt(eval, inst->rhs);
}

U64 evalXor(Eval* eval, InstXor* inst) {
    return evalInt(eval, inst->lhs) ^ evalInt(eval, inst->rhs);
}

U64 evalAlloc(Eval* eval, InstAlloc* inst) {
    auto ref = (RefType*)inst->type;
    auto size = inst->valueType->virtualSize;

    if(ref->isLocal) {
        auto p = eval->sp;
        eval->sp += size;
        return asReg(p);
    } else {
        auto allocSize = size * sizeof(U64);
        eval->heapSize += allocSize;
        if(eval->heapSize > eval->heapMax) {
            raise(eval, NoMemory);
        }
        return asReg(eval->heap.alloc(allocSize));
    }
}

U64 evalLoad(Eval* eval, InstLoad* inst) {
    auto type = (RefType*)inst->type;
    auto ptr = (U64*)evalValue(eval, inst->from);
    auto size = type->virtualSize;

    if(size > 1) {
        auto p = eval->sp;
        eval->sp += size;
        memcpy(p, ptr, sizeof(U64) * size);
    } else {
        return *ptr;
    }
}

U64 evalStore(Eval* eval, InstStore* inst) {
    auto type = (RefType*)inst->type;
    auto value = evalValue(eval, inst->value);
    auto ptr = (U64*)evalValue(eval, inst->to);
    auto size = type->virtualSize;

    if(size > 1) {
        memcpy(ptr, (U64*)value, sizeof(U64) * size);
    } else {
        *ptr = value;
    }

    return 0;
}

U64 evalCall(Eval* eval, InstCall* call) {
    auto args = call->args;
    auto count = call->argCount;
    auto fun = call->fun;
    auto returnSize = fun->returnType->virtualSize;

    if(returnSize > 1) {
        eval->sp += returnSize;
    }

    pushp(eval, eval->bp + call->id);

    for(U32 i = 0; i < count; i++) {
        push64(eval, evalValue(eval, args[i]));
    }

    funIntro(eval, fun);
    return 0;
}

U64 evalJmp(Eval* eval, InstJmp* inst) {
    eval->lb = inst->block;
    setBlock(eval, inst->to);
}

U64 evalJe(Eval* eval, InstJe* inst) {
    eval->lb = inst->block;
    auto v = evalInt(eval, inst->cond);
    if(v) {
        setBlock(eval, inst->then);
    } else {
        setBlock(eval, inst->otherwise);
    }
    return 0;
}

U64 evalRet(Eval* eval, InstRet* inst) {
    auto ap = eval->ap;
    auto reg = *(U64**)(ap - 1);
    auto value = evalValue(eval, inst->value);
    auto size = inst->value->type->virtualSize;

    if(size > 1) {
        memcpy(ap - size - 1, (U64*)value, size * sizeof(U64));
        *reg = (U64)(ap - size - 1);
    } else {
        *reg = value;
    }

    eval->sp = eval->bp;
    eval->ap = popp(eval);
    eval->bp = popp(eval);
    eval->pc = (Inst**)popp(eval);
    eval->sp = ap - 1;

    if(eval->sp <= eval->sb) {
        raise(eval, Exit);
    }

    return 0;
}

U64 evalPhi(Eval* eval, InstPhi* inst) {
    auto lb = eval->lb;
    auto alts = inst->alts;
    for(U32 i = 0; i < inst->altCount; i++) {
        if(lb == alts[i].fromBlock) {
            return evalValue(eval, alts[i].value);
        }
    }

    raise(eval, InvalidOp);
}

typedef U64 (*inst)(Eval* eval, Inst* inst);

static const inst instructions[] = {
    (inst)evalNop, // InstNop,
    (inst)evalTrunc, // InstTrunc,
    (inst)evalFTrunc, // InstFTrunc,
    (inst)evalZExt, // InstZExt,
    (inst)evalSExt, // InstSExt,
    (inst)evalFExt, // InstFExt,
    (inst)evalFToI, // InstFToI,
    (inst)evalFToUI, // InstFToUI,
    (inst)evalIToF, // InstIToF,
    (inst)evalUIToF, // InstUIToF,

    // Primitives: arithmetic.
    (inst)evalAdd, // InstAdd,
    (inst)evalSub, // InstSub,
    (inst)evalMul, // InstMul,
    (inst)evalDiv, // InstDiv,
    (inst)evalIDiv, // InstIDiv,
    (inst)evalRem, // InstRem,
    (inst)evalIRem, // InstIRem,
    (inst)evalFAdd, // InstFAdd,
    (inst)evalFSub, // InstFSub,
    (inst)evalFMul, // InstFMul,
    (inst)evalFDiv, // InstFDiv,

    (inst)evalICmp, // InstICmp,
    (inst)evalFCmp, // InstFCmp,

    (inst)evalShl, // InstShl,
    (inst)evalShr, // InstShr,
    (inst)evalSar, // InstSar,
    (inst)evalAnd, // InstAnd,
    (inst)evalOr, // InstOr,
    (inst)evalXor, // InstXor,

    // Construction.
    nullptr, // InstRecord,
    nullptr, // InstTup,
    nullptr, // InstFun,

    // Memory.
    (inst)evalAlloc, // InstAlloc,
    (inst)evalLoad, // InstLoad,
    nullptr, // InstLoadField,
    nullptr, // InstLoadGlobal,
    (inst)evalStore, // InstStore,
    nullptr, // InstStoreField,
    nullptr, // InstStoreGlobal,

    nullptr, // InstGetField,
    nullptr, // InstUpdateField,

    // Function calls.
    (inst)evalCall, // InstCall,
    nullptr, // InstCallGen,
    nullptr, // InstCallDyn,
    nullptr, // InstCallDynGen,
    nullptr, // InstCallForeign,

    // Control flow.
    (inst)evalJe, // InstJe,
    (inst)evalJmp, // InstJmp,
    (inst)evalRet, // InstRet,
    (inst)evalPhi, // InstPhi,
};

void evalInst(Eval* eval, Inst* i) {
    eval->bp[i->id] = instructions[i->kind - Value::FirstInst](eval, i);
}

void runEval(Eval* eval) {
    while(1) {
        evalInst(eval, *eval->pc);
        eval->pc++;
    }
}

void evalFun(Function* fun) {
    U64 stack[2 * 1024];

    Eval eval;
    eval.heapSize = 0;
    eval.heapMax = 10 * 1024;
    eval.sp = stack;
    eval.sb = stack;
    eval.bp = nullptr;
    eval.ap = nullptr;
    eval.lb = nullptr;
    setBlock(&eval, fun->blocks[0]);

    jmp_buf raiseBuffer;
    if(auto exception = setjmp(raiseBuffer)) {
        if(exception == Exit) {

        }
    } else {
        eval.raise = &raiseBuffer;
        runEval(&eval);
    }
}