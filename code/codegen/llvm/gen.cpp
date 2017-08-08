#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include "gen.h"
#include "../../resolve/inst.h"
#include "../../resolve/block.h"
#include "../../resolve/type.h"
#include <llvm/IR/Module.h>

llvm::Type* genIntType(Gen* gen, IntType* type) {
    switch(type->width) {
        case IntType::Bool:
            return gen->builder->getInt1Ty();
        case IntType::Int:
            return gen->builder->getInt32Ty();
        case IntType::Long:
        default:
            return gen->builder->getInt64Ty();
    }
}

llvm::Type* genFloatType(Gen* gen, FloatType* type) {
    switch(type->width) {
        case FloatType::F16:
            return gen->builder->getHalfTy();
        case FloatType::F32:
            return gen->builder->getFloatTy();
        case FloatType::F64:
        default:
            return gen->builder->getDoubleTy();
    }
}

llvm::Type* genType(Gen* gen, Type* type) {
    switch(type->kind) {
        case Type::Error:
        case Type::Unit:
            return gen->builder->getVoidTy();
        case Type::Gen:
            return gen->builder->getInt8PtrTy(0);
        case Type::Int:
            return genIntType(gen, (IntType*)type);
        case Type::Float:
            return genFloatType(gen, (FloatType*)type);
        case Type::String:
            return gen->builder->getInt8PtrTy(0);
        case Type::Ref:
            return llvm::PointerType::get(genType(gen, ((RefType*)type)->to), 0);
        case Type::Fun:
        case Type::Array:
        case Type::Map:
        case Type::Tup:
        case Type::Record:
            return gen->builder->getInt8PtrTy(0);
        case Type::Alias:
            return genType(gen, ((AliasType*)type)->to);
    }
    return nullptr;
}

llvm::Value* useValue(Gen* gen, Value* value) {
    auto v = (llvm::Value*)value->codegen;
    if(v) return v;

    switch(value->kind) {
        case Value::ConstInt:
            return llvm::ConstantInt::get(genType(gen, value->type), ((ConstInt*)value)->value);
        case Value::ConstFloat:
            return llvm::ConstantFP::get(genType(gen, value->type), ((ConstFloat*)value)->value);
    }

    // If this happens, some instruction is not generated correctly.
    return nullptr;
}

llvm::Value* genTrunc(Gen* gen, InstTrunc* inst) {
    return gen->builder->CreateTrunc(useValue(gen, inst->from), genType(gen, inst->type));
}

llvm::Value* genFTrunc(Gen* gen, InstFTrunc* inst) {
    return gen->builder->CreateFPTrunc(useValue(gen, inst->from), genType(gen, inst->type));
}

llvm::Value* genZExt(Gen* gen, InstZExt* inst) {
    return gen->builder->CreateZExt(useValue(gen, inst->from), genType(gen, inst->type));
}

llvm::Value* genSExt(Gen* gen, InstSExt* inst) {
    return gen->builder->CreateSExt(useValue(gen, inst->from), genType(gen, inst->type));
}

llvm::Value* genFExt(Gen* gen, InstFExt* inst) {
    return gen->builder->CreateFPExt(useValue(gen, inst->from), genType(gen, inst->type));
}

llvm::Value* genAdd(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateAdd(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genSub(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateSub(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genMul(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateMul(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genDiv(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateUDiv(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genIDiv(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateSDiv(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genRem(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateURem(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genIRem(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateSRem(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genFAdd(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateFAdd(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genFSub(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateFSub(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genFMul(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateFMul(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genFDiv(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateFDiv(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genICmp(Gen* gen, InstICmp* inst) {
    llvm::CmpInst::Predicate p;
    switch(inst->cmp) {
        case ICmp::eq:
            p = llvm::CmpInst::ICMP_EQ;
            break;
        case ICmp::neq:
            p = llvm::CmpInst::ICMP_NE;
            break;
        case ICmp::gt:
            p = llvm::CmpInst::ICMP_UGT;
            break;
        case ICmp::ge:
            p = llvm::CmpInst::ICMP_UGE;
            break;
        case ICmp::lt:
            p = llvm::CmpInst::ICMP_ULT;
            break;
        case ICmp::le:
            p = llvm::CmpInst::ICMP_ULE;
            break;
        case ICmp::igt:
            p = llvm::CmpInst::ICMP_SGT;
            break;
        case ICmp::ige:
            p = llvm::CmpInst::ICMP_SGE;
            break;
        case ICmp::ilt:
            p = llvm::CmpInst::ICMP_SLT;
            break;
        case ICmp::ile:
            p = llvm::CmpInst::ICMP_SLE;
            break;
    }
    return gen->builder->CreateICmp(p, useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genFCmp(Gen* gen, InstFCmp* inst) {
    llvm::CmpInst::Predicate p;
    switch(inst->cmp) {
        case FCmp::eq:
            p = llvm::CmpInst::FCMP_OEQ;
            break;
        case FCmp::neq:
            p = llvm::CmpInst::FCMP_ONE;
            break;
        case FCmp::gt:
            p = llvm::CmpInst::FCMP_OGT;
            break;
        case FCmp::ge:
            p = llvm::CmpInst::FCMP_OGE;
            break;
        case FCmp::lt:
            p = llvm::CmpInst::FCMP_OLT;
            break;
        case FCmp::le:
            p = llvm::CmpInst::FCMP_OLE;
            break;
    }
    return gen->builder->CreateICmp(p, useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genShl(Gen* gen, InstShift* inst) {
    return gen->builder->CreateShl(useValue(gen, inst->arg), useValue(gen, inst->amount));
}

llvm::Value* genShr(Gen* gen, InstShift* inst) {
    return gen->builder->CreateLShr(useValue(gen, inst->arg), useValue(gen, inst->amount));
}

llvm::Value* genSar(Gen* gen, InstShift* inst) {
    return gen->builder->CreateAShr(useValue(gen, inst->arg), useValue(gen, inst->amount));
}

llvm::Value* genAnd(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateAnd(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genOr(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateOr(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genXor(Gen* gen, InstBinary* inst) {
    return gen->builder->CreateXor(useValue(gen, inst->lhs), useValue(gen, inst->rhs));
}

llvm::Value* genJe(Gen* gen, InstJe* inst) {
    auto then = (llvm::BasicBlock*)inst->then->codegen;
    auto otherwise = (llvm::BasicBlock*)inst->otherwise->codegen;
    return gen->builder->CreateCondBr(useValue(gen, inst->cond), then, otherwise);
}

llvm::Value* genJmp(Gen* gen, InstJmp* inst) {
    auto to = (llvm::BasicBlock*)inst->to->codegen;
    return gen->builder->CreateBr(to);
}

llvm::Value* genRet(Gen* gen, InstRet* inst) {
    auto value = inst->value ? useValue(gen, inst->value) : nullptr;
    return gen->builder->CreateRet(value);
}

llvm::Value* genPhi(Gen* gen, InstPhi* inst) {
    auto phi = gen->builder->CreatePHI(genType(gen, inst->type), (U32)inst->altCount);
    for(Size i = 0; i < inst->altCount; i++) {
        auto& alt = inst->alts[i];
        phi->addIncoming(useValue(gen, alt.value), (llvm::BasicBlock*)alt.fromBlock->codegen);
    }
    return phi;
}

llvm::Value* genInstValue(Gen* gen, Inst* inst) {
    switch(inst->kind) {
        case Inst::InstTrunc:
            return genTrunc(gen, (InstTrunc*)inst);
        case Inst::InstFTrunc:
            return genFTrunc(gen, (InstFTrunc*)inst);
        case Inst::InstZExt:
            return genZExt(gen, (InstZExt*)inst);
        case Inst::InstSExt:
            return genSExt(gen, (InstSExt*)inst);
        case Inst::InstFExt:
            return genFExt(gen, (InstFExt*)inst);
        case Inst::InstAdd:
            return genAdd(gen, (InstAdd*)inst);
        case Inst::InstSub:
            return genSub(gen, (InstSub*)inst);
        case Inst::InstMul:
            return genMul(gen, (InstMul*)inst);
        case Inst::InstDiv:
            return genDiv(gen, (InstDiv*)inst);
        case Inst::InstIDiv:
            return genIDiv(gen, (InstIDiv*)inst);
        case Inst::InstRem:
            return genRem(gen, (InstRem*)inst);
        case Inst::InstIRem:
            return genIRem(gen, (InstIRem*)inst);
        case Inst::InstFAdd:
            return genFAdd(gen, (InstFAdd*)inst);
        case Inst::InstFSub:
            return genFSub(gen, (InstFSub*)inst);
        case Inst::InstFMul:
            return genFMul(gen, (InstFMul*)inst);
        case Inst::InstFDiv:
            return genFDiv(gen, (InstFDiv*)inst);
        case Inst::InstICmp:
            return genICmp(gen, (InstICmp*)inst);
        case Inst::InstFCmp:
            return genFCmp(gen, (InstFCmp*)inst);
        case Inst::InstShl:
            return genShl(gen, (InstShift*)inst);
        case Inst::InstShr:
            return genShr(gen, (InstShift*)inst);
        case Inst::InstSar:
            return genSar(gen, (InstShift*)inst);
        case Inst::InstAnd:
            return genAnd(gen, (InstAnd*)inst);
        case Inst::InstOr:
            return genOr(gen, (InstOr*)inst);
        case Inst::InstXor:
            return genXor(gen, (InstXor*)inst);
        case Inst::InstJe:
            return genJe(gen, (InstJe*)inst);
        case Inst::InstJmp:
            return genJmp(gen, (InstJmp*)inst);
        case Inst::InstRet:
            return genRet(gen, (InstRet*)inst);
        case Inst::InstPhi:
            return genPhi(gen, (InstPhi*)inst);
    }
    return nullptr;
}

llvm::Value* genInst(Gen* gen, Inst* inst) {
    auto value = genInstValue(gen, inst);
    inst->codegen = value;
    return value;
}