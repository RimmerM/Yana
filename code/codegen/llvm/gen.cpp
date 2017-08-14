#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include "gen.h"
#include <llvm/IR/Module.h>

llvm::BasicBlock* genBlock(Gen* gen, Block* block);
llvm::Function* genFunction(Gen* gen, Function* fun);
llvm::Type* useType(Gen* gen, Type* type);

static llvm::StringRef toRef(Context* context, Id name) {
    if(name == 0) return "";

    auto& v = context->find(name);
    if(v.textLength == 0) return "";

    return {v.text, v.textLength};
}

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

llvm::Type* genTupType(Gen* gen, TupType* type) {
    auto types = (llvm::Type**)alloca(sizeof(llvm::Type*) * type->count);
    for(U32 i = 0; i < type->count; i++) {
        types[i] = useType(gen, type->layout[i]);
    }

    auto t = llvm::StructType::get(*gen->llvm, {types, type->count}, false);
    return t;
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
            return llvm::PointerType::get(useType(gen, ((RefType*)type)->to), 0);
        case Type::Fun:
        case Type::Array:
        case Type::Map:
            return gen->builder->getInt8PtrTy(0);
        case Type::Tup:
            return genTupType(gen, (TupType*)type);
        case Type::Record:
            return gen->builder->getInt8PtrTy(0);
        case Type::Alias:
            return useType(gen, ((AliasType*)type)->to);
    }
    return nullptr;
}

llvm::Type* useType(Gen* gen, Type* type) {
    auto v = type->codegen;
    if(v) return (llvm::Type*)v;

    auto t = genType(gen, type);
    type->codegen = t;
    return t;
}

llvm::Function* useFunction(Gen* gen, Function* fun) {
    auto v = (llvm::Function*)fun->codegen;
    if(v) return v;

    return genFunction(gen, fun);
}

llvm::BasicBlock* useBlock(Gen* gen, Block* block) {
    auto v = (llvm::BasicBlock*)block->codegen;
    if(v) return v;

    return genBlock(gen, block);
}

llvm::Value* useValue(Gen* gen, Value* value) {
    auto v = (llvm::Value*)value->codegen;
    if(v) return v;

    switch(value->kind) {
        case Value::ConstInt:
            return llvm::ConstantInt::get(useType(gen, value->type), ((ConstInt*)value)->value);
        case Value::ConstFloat:
            return llvm::ConstantFP::get(useType(gen, value->type), ((ConstFloat*)value)->value);
    }

    // If this happens, some instruction is not generated correctly.
    return nullptr;
}

llvm::Value* genTrunc(Gen* gen, InstTrunc* inst) {
    return gen->builder->CreateTrunc(useValue(gen, inst->from), useType(gen, inst->type));
}

llvm::Value* genFTrunc(Gen* gen, InstFTrunc* inst) {
    return gen->builder->CreateFPTrunc(useValue(gen, inst->from), useType(gen, inst->type));
}

llvm::Value* genZExt(Gen* gen, InstZExt* inst) {
    return gen->builder->CreateZExt(useValue(gen, inst->from), useType(gen, inst->type));
}

llvm::Value* genSExt(Gen* gen, InstSExt* inst) {
    return gen->builder->CreateSExt(useValue(gen, inst->from), useType(gen, inst->type));
}

llvm::Value* genFExt(Gen* gen, InstFExt* inst) {
    return gen->builder->CreateFPExt(useValue(gen, inst->from), useType(gen, inst->type));
}

llvm::Value* genFToI(Gen* gen, InstFToI* inst) {
    return gen->builder->CreateFPToSI(useValue(gen, inst->from), useType(gen, inst->type));
}

llvm::Value* genFToUI(Gen* gen, InstFToUI* inst) {
    return gen->builder->CreateFPToUI(useValue(gen, inst->from), useType(gen, inst->type));
}

llvm::Value* genIToF(Gen* gen, InstIToF* inst) {
    return gen->builder->CreateSIToFP(useValue(gen, inst->from), useType(gen, inst->type));
}

llvm::Value* genUIToF(Gen* gen, InstUIToF* inst) {
    return gen->builder->CreateUIToFP(useValue(gen, inst->from), useType(gen, inst->type));
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

llvm::Value* genTup(Gen* gen, InstTup* inst) {
    auto type = useType(gen, inst->type);
    llvm::Value* tup = llvm::UndefValue::get(type);

    for(U32 i = 0; i < inst->fieldCount; i++) {
        tup = gen->builder->CreateInsertValue(tup, useValue(gen, inst->fields[i]), i);
    }

    return tup;
}

llvm::Value* genAlloc(Gen* gen, InstAlloc* inst) {
    auto type = (RefType*)inst->type;
    if(type->isLocal) {
        return gen->builder->CreateAlloca(useType(gen, type->to));
    } else {
        // TODO
        return nullptr;
    }
}

llvm::Value* genLoad(Gen* gen, InstLoad* inst) {
    return gen->builder->CreateLoad(useValue(gen, inst->from));
}

llvm::Value* genStore(Gen* gen, InstStore* inst) {
    return gen->builder->CreateStore(useValue(gen, inst->value), useValue(gen, inst->to));
}

llvm::Value* genGetField(Gen* gen, InstGetField* inst) {
    return gen->builder->CreateExtractValue(useValue(gen, inst->from), {inst->indexChain, inst->chainLength});
}

llvm::Value* genCall(Gen* gen, InstCall* inst) {
    auto args = (llvm::Value**)alloca(sizeof(llvm::Value*) * inst->argCount);
    for(U32 i = 0; i < inst->argCount; i++) {
        args[i] = useValue(gen, inst->args[i]);
    }

    auto call = gen->builder->CreateCall(useFunction(gen, inst->fun), {args, inst->argCount});
    call->setCallingConv(llvm::CallingConv::Fast);
    return call;
}

llvm::Value* genJe(Gen* gen, InstJe* inst) {
    auto then = useBlock(gen, inst->then);
    auto otherwise = useBlock(gen, inst->otherwise);
    return gen->builder->CreateCondBr(useValue(gen, inst->cond), then, otherwise);
}

llvm::Value* genJmp(Gen* gen, InstJmp* inst) {
    auto to = useBlock(gen, inst->to);
    return gen->builder->CreateBr(to);
}

llvm::Value* genRet(Gen* gen, InstRet* inst) {
    auto value = inst->value ? useValue(gen, inst->value) : nullptr;
    return gen->builder->CreateRet(value);
}

llvm::Value* genPhi(Gen* gen, InstPhi* inst) {
    auto phi = gen->builder->CreatePHI(useType(gen, inst->type), (U32)inst->altCount);
    inst->codegen = phi;

    for(Size i = 0; i < inst->altCount; i++) {
        auto& alt = inst->alts[i];
        auto block = useBlock(gen, alt.fromBlock);
        phi->addIncoming(useValue(gen, alt.value), block);
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
        case Inst::InstFToI:
            return genFToI(gen, (InstFToI*)inst);
        case Inst::InstFToUI:
            return genFToUI(gen, (InstFToUI*)inst);
        case Inst::InstIToF:
            return genIToF(gen, (InstIToF*)inst);
        case Inst::InstUIToF:
            return genUIToF(gen, (InstUIToF*)inst);
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
        case Inst::InstTup:
            return genTup(gen, (InstTup*)inst);
        case Inst::InstAlloc:
            return genAlloc(gen, (InstAlloc*)inst);
        case Inst::InstLoad:
            return genLoad(gen, (InstLoad*)inst);
        case Inst::InstStore:
            return genStore(gen, (InstStore*)inst);
        case Inst::InstGetField:
            return genGetField(gen, (InstGetField*)inst);
        case Inst::InstCall:
            return genCall(gen, (InstCall*)inst);
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

llvm::BasicBlock* genBlock(Gen* gen, Block* block) {
    auto b = llvm::BasicBlock::Create(*gen->llvm, "", (llvm::Function*)block->function->codegen);
    block->codegen = b;

    auto builder = gen->builder;
    auto insertBlock = builder->GetInsertBlock();
    auto insert = builder->GetInsertPoint();

    builder->SetInsertPoint(b);
    for(auto inst: block->instructions) {
        genInst(gen, inst);
    }

    if(insertBlock) {
        builder->SetInsertPoint(insertBlock, insert);
    }
    return b;
}

llvm::Function* genFunction(Gen* gen, Function* fun) {
    auto ret = useType(gen, fun->returnType);
    auto argCount = fun->args.size();
    auto args = (llvm::Type**)alloca(argCount * sizeof(llvm::Type*));
    for(U32 i = 0; i < argCount; i++) {
        args[i] = useType(gen, fun->args[i].type);
    }

    auto sig = llvm::FunctionType::get(ret, llvm::ArrayRef<llvm::Type*>(args, argCount), false);
    auto linkage = llvm::Function::ExternalLinkage;
    auto f = llvm::Function::Create(sig, linkage, toRef(gen->context, fun->name), gen->module);

    f->setCallingConv(llvm::CallingConv::Fast);
    fun->codegen = f;

    U32 i = 0;
    for(auto it = f->arg_begin(); it != f->arg_end(); i++, it++) {
        auto arg = &fun->args[i];
        it->setName(toRef(gen->context, arg->name));
        arg->codegen = &*it;
    }

    genBlock(gen, fun->blocks[0]);
    return f;
}

llvm::Value* genGlobal(Gen* gen, Global* global) {
    auto name = toRef(gen->context, global->name);
    auto type = useType(gen, ((RefType*)global->type)->to);
    auto g = new llvm::GlobalVariable(*gen->module, type, false, llvm::GlobalVariable::ExternalLinkage, nullptr,
                                      name, nullptr, llvm::GlobalVariable::NotThreadLocal, 0, true);
    global->codegen = g;
    return g;
}

llvm::Module* genModule(llvm::LLVMContext* llvm, Context* context, Module* module) {
    auto name = llvm::StringRef{module->name->text, module->name->textLength};
    auto llvmModule = new llvm::Module(name, *llvm);
    llvmModule->setDataLayout("e-S128");
    llvmModule->setTargetTriple(LLVM_HOST_TRIPLE);

    llvm::IRBuilder<> builder(*llvm);
    Gen gen{llvm, llvmModule, &builder, context};

    for(auto& global: module->globals) {
        genGlobal(&gen, &global);
    }

    for(auto& fun: module->functions) {
        // Functions can be lazily generated depending on their usage order, so we only generate if needed.
        useFunction(&gen, &fun);
    }

    return llvmModule;
}