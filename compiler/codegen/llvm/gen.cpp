#include "gen.h"
#include <llvm/IR/Module.h>
#include <llvm/IR/InlineAsm.h>

llvm::BasicBlock* genBlock(Gen* gen, Block* block);
llvm::Function* genFunction(Gen* gen, Function* fun);
llvm::Type* useType(Gen* gen, Type* type);

struct RecordGen {
    llvm::Type* selectorType;
    llvm::Type* opaqueType;
    U32 selectorSize;
    U32 selectorAlignment;
    U32 maxSize;
    U32 maxAlignment;
    bool indirect;
};

struct ConGen {
    llvm::Type* memType;
    llvm::Type* regType;
    U32 size;
    U32 alignment;
};

static llvm::StringRef toRef(Context* context, Id name) {
    if(name == 0) return "";

    auto& v = context->find(name);
    if(v.textLength == 0) return "";

    return {v.text, v.textLength};
}

static bool isIndirect(Type* type) {
    type = canonicalType(type);
    return type->kind == Type::Record && ((RecordGen*)type->codegen)->indirect;
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
        types[i] = useType(gen, type->fields[i].type);
    }

    auto t = llvm::StructType::get(*gen->llvm, {types, type->count}, false);
    return t;
}

RecordGen* genRecordType(Gen* gen, RecordType* type) {
    auto layout = &gen->module->getDataLayout();
    auto conCount = type->conCount;
    auto cons = type->cons;
    auto record = new (*gen->mem) RecordGen;
    record->indirect = false;

    if(conCount > 1) {
        if(conCount <= 255) {
            record->selectorType = gen->builder->getInt8Ty();
        } else if(conCount <= 65535) {
            record->selectorType = gen->builder->getInt16Ty();
        } else {
            record->selectorType = gen->builder->getInt32Ty();
        }

        record->selectorSize = (U32)layout->getTypeAllocSize(record->selectorType);
        record->selectorAlignment = layout->getPrefTypeAlignment(record->selectorType);
    } else {
        record->selectorType = nullptr;
        record->selectorSize = 0;
        record->selectorAlignment = 0;
    }

    if(type->kind == RecordType::Enum) {
        record->maxSize = record->selectorSize;
        record->maxAlignment = record->selectorAlignment;
        record->opaqueType = record->selectorType;

        auto conGen = new (*gen->mem) ConGen;
        conGen->regType = record->selectorType;
        conGen->memType = record->selectorType;
        conGen->alignment = record->selectorAlignment;
        conGen->size = record->selectorSize;

        for(U32 i = 0; i < conCount; i++) {
            cons[i].codegen = conGen;
        }
    } else if(type->kind == RecordType::Single) {
        auto conGen = new (*gen->mem) ConGen;
        cons[0].codegen = conGen;

        llvm::Type* con = useType(gen, cons[0].content);
        conGen->memType = con;
        conGen->regType = con;

        auto s = (U32)layout->getTypeAllocSize(con);
        auto a = layout->getPrefTypeAlignment(con);
        conGen->size = s;
        conGen->alignment = a;

        record->opaqueType = con;
        record->maxAlignment = a;
        record->maxSize = s;
    } else {
        U32 maxSize = 0;
        U32 maxAlignment = 0;

        for(U32 i = 0; i < conCount; i++) {
            auto conGen = new (*gen->mem) ConGen;
            cons[i].codegen = conGen;

            if(cons[i].content) {
                llvm::Type* con = useType(gen, cons[i].content);

                llvm::Type* memTypes[2];
                memTypes[0] = record->selectorType;
                memTypes[1] = con;
                auto t = llvm::StructType::get(*gen->llvm, {memTypes, 2});
                conGen->memType = t;

                auto s = (U32)layout->getTypeAllocSize(t);
                auto a = layout->getPrefTypeAlignment(t);
                conGen->size = s;
                conGen->alignment = a;

                maxSize = std::max(maxSize, s);
                maxAlignment = std::max(maxAlignment, a);
            } else {
                conGen->regType = record->selectorType;
                conGen->memType = record->selectorType;
                conGen->size = record->selectorSize;
                conGen->alignment = record->selectorAlignment;
            }
        }

        record->maxAlignment = maxAlignment;
        record->maxSize = maxSize;

        if(conCount > 1) {
            for(U32 i = 0; i < conCount; i++) {
                auto conGen = (ConGen*)cons[i].codegen;

                if(conGen->size < maxSize) {
                    if(conGen->memType == record->selectorType) {
                        llvm::Type* regTypes[2];
                        regTypes[0] = record->selectorType;
                        regTypes[1] = llvm::ArrayType::get(gen->builder->getInt8Ty(), maxSize - conGen->size);
                        conGen->regType = llvm::StructType::get(*gen->llvm, {regTypes, 2});
                    } else {
                        llvm::Type* regTypes[3];
                        regTypes[0] = record->selectorType;
                        regTypes[1] = conGen->memType;
                        regTypes[2] = llvm::ArrayType::get(gen->builder->getInt8Ty(), maxSize - conGen->size);
                        conGen->regType = llvm::StructType::get(*gen->llvm, {regTypes, 3});
                    }
                } else {
                    conGen->regType = conGen->memType;
                }

                if(i > 0 && conGen->regType != ((ConGen*)cons[i - 1].codegen)->regType) {
                    record->indirect = true;
                }
            }

            record->opaqueType = ((ConGen*)cons[0].codegen)->regType;
        } else {
            auto conGen = (ConGen*)cons[0].codegen;
            conGen->regType = conGen->memType;
            record->opaqueType = conGen->memType;
        }
    }

    return record;
}

llvm::Type* genStringType(llvm::LLVMContext* context) {
    auto length = llvm::IntegerType::getInt32Ty(*context);
    auto data = llvm::ArrayType::get(llvm::IntegerType::getInt8Ty(*context), 0);
    llvm::Type* fields[2];
    fields[0] = length;
    fields[1] = data;

    return llvm::StructType::get(*context, {fields, 2});
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
            return gen->types.stringRef;
        case Type::Ref:
            return llvm::PointerType::get(useType(gen, ((RefType*)type)->to), 0);
        case Type::Fun:
        case Type::Array:
        case Type::Map:
            return gen->builder->getInt8PtrTy(0);
        case Type::Tup:
            return genTupType(gen, (TupType*)type);
        case Type::Alias:
            return useType(gen, ((AliasType*)type)->to);
    }
    return nullptr;
}

llvm::Type* useType(Gen* gen, Type* type) {
    auto v = type->codegen;
    if(v) {
        if(type->kind == Type::Record) {
            return ((RecordGen*)v)->opaqueType;
        } else {
            return (llvm::Type*)v;
        }
    }

    if(type->kind == Type::Record) {
        auto t = genRecordType(gen, (RecordType*)type);
        type->codegen = t;
        return t->opaqueType;
    } else {
        auto t = genType(gen, type);
        type->codegen = t;
        return t;
    }
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

llvm::Value* genStringLiteral(Gen* gen, const char* v, U32 l) {
    auto type = gen->types.stringRef;
    llvm::Constant* values[2];
    values[0] = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*gen->llvm), l);
    values[1] = llvm::ConstantDataArray::getString(*gen->llvm, {v, l}, false);

    llvm::Type* types[2];
    types[0] = values[0]->getType();
    types[1] = values[1]->getType();

    auto stringType = llvm::StructType::get(*gen->llvm, {types, 2});
    auto initializer = llvm::ConstantStruct::get(stringType, {values, 2});
    auto var = new llvm::GlobalVariable(*gen->module, stringType, true, llvm::GlobalValue::PrivateLinkage, initializer);
    return gen->builder->CreateBitCast(var, type);
}

llvm::Value* useValue(Gen* gen, Value* value) {
    auto v = (llvm::Value*)value->codegen;
    if(v) return v;

    switch(value->kind) {
        case Value::ConstInt:
            return llvm::ConstantInt::get(useType(gen, value->type), ((ConstInt*)value)->value);
        case Value::ConstFloat:
            return llvm::ConstantFP::get(useType(gen, value->type), ((ConstFloat*)value)->value);
        case Value::ConstString:
            return genStringLiteral(gen, ((ConstString*)value)->value, ((ConstString*)value)->length);
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
    return gen->builder->CreateFCmp(p, useValue(gen, inst->lhs), useValue(gen, inst->rhs));
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

llvm::Value* genRecord(Gen* gen, InstRecord* inst) {
    useType(gen, inst->type);
    auto type = (RecordGen*)inst->type->codegen;
    auto con = (ConGen*)inst->con->codegen;

    if(type->selectorType && type->selectorType == type->opaqueType) {
        return llvm::ConstantInt::get(type->selectorType, inst->con->index);
    } else if(type->selectorType) {
        auto conIndex = llvm::ConstantInt::get(type->selectorType, inst->con->index);
        auto v = useValue(gen, inst->content);
        if(isIndirect(inst->content->type)) {
            v = gen->builder->CreateLoad(v);
        }

        if(type->indirect) {
            llvm::Value* record = gen->builder->CreateAlloca(con->regType);

            llvm::Value* indices[2];
            indices[0] = gen->builder->getInt32(0);
            indices[1] = indices[0];
            auto selector = gen->builder->CreateGEP(record, {indices, 2});
            gen->builder->CreateStore(conIndex, selector);

            indices[1] = gen->builder->getInt32(1);
            auto content = gen->builder->CreateGEP(record, {indices, 2});
            gen->builder->CreateStore(v, content);

            return record;
        } else {
            llvm::Value* record = llvm::UndefValue::get(con->regType);
            record = gen->builder->CreateInsertValue(record, conIndex, 0);
            record = gen->builder->CreateInsertValue(record, v, 1);
            return record;
        }
    } else {
        // If the content type is indirect, we continue using the same value when possible.
        // The IR will contain an explicit alloc + store if the copied value is mutable.
        return useValue(gen, inst->content);
    }
}

llvm::Value* genTup(Gen* gen, InstTup* inst) {
    auto type = useType(gen, inst->type);
    llvm::Value* tup = llvm::UndefValue::get(type);

    for(U32 i = 0; i < inst->fieldCount; i++) {
        auto v = useValue(gen, inst->fields[i]);
        if(isIndirect(inst->fields[i]->type)) {
            v = gen->builder->CreateLoad(v);
        }
        tup = gen->builder->CreateInsertValue(tup, v, i);
    }

    return tup;
}

llvm::Value* genAlloc(Gen* gen, InstAlloc* inst) {
    auto type = (RefType*)canonicalType(inst->type);
    if(type->isLocal) {
        return gen->builder->CreateAlloca(useType(gen, type->to));
    } else {
        // TODO
        return nullptr;
    }
}

llvm::Value* genLoad(Gen* gen, InstLoad* inst) {
    // TODO: For records - load from mem form into reg form.
    if(isIndirect(inst->from->type)) {
        return useValue(gen, inst->from);
    } else {
        return gen->builder->CreateLoad(useValue(gen, inst->from));
    }
}

llvm::Value* genStore(Gen* gen, InstStore* inst) {
    // TODO: For records - store from reg form into mem form.
    if(isIndirect(inst->value->type)) {
        auto v = gen->builder->CreateLoad(useValue(gen, inst->value));
        return gen->builder->CreateStore(v, useValue(gen, inst->to));
    } else {
        return gen->builder->CreateStore(useValue(gen, inst->value), useValue(gen, inst->to));
    }
}

static llvm::Value* buildChain(Gen* gen, llvm::Value* from, U32* chain, U32& chainLength) {
    auto v = gen->builder->CreateExtractValue(from, {chain, chainLength});
    chainLength = 0;
    return v;
}

llvm::Value* genGetField(Gen* gen, InstGetField* inst) {
    auto currentChain = (U32*)alloca(sizeof(U32) * inst->chainLength);
    U32 chainLength = 0;

    auto from = canonicalType(inst->from->type);
    llvm::Value* result = useValue(gen, inst->from);

    for(U32 i = 0; i < inst->chainLength; i++) {
        U32 index = inst->indexChain[i];
        if(from->kind == Type::Record) {
            // Record type: either get the constructor id or cast to a specific constructor.
            // Start with lazily generating any previous tuple chain.
            if(chainLength) {
                result = buildChain(gen, result, currentChain, chainLength);
            }

            useType(gen, from);
            auto recordType = (RecordType*)from;
            auto recordGen = (RecordGen*)recordType->codegen;

            if(index == 0) {
                if(recordGen->indirect) {
                    if(recordGen->selectorType && recordGen->selectorType == recordGen->opaqueType) {
                        result = gen->builder->CreateLoad(result);
                    } else if(recordGen->selectorType) {
                        llvm::Value* indices[2];
                        indices[0] = gen->builder->getInt32(0);
                        indices[1] = indices[0];
                        auto p = gen->builder->CreateGEP(result, {indices, 2});

                        result = gen->builder->CreateLoad(p);
                    } else {
                        result = gen->builder->getInt32(0);
                    }
                } else {
                    if(recordGen->selectorType && recordGen->selectorType == recordGen->opaqueType) {
                        // Do nothing - result already has the correct type.
                    } else if(recordGen->selectorType) {
                        result = gen->builder->CreateExtractValue(result, {&index, 1});
                    } else {
                        result = gen->builder->getInt32(0);
                    }
                }

                auto targetType = gen->builder->getInt32Ty();
                if(result->getType() != targetType) {
                    result = gen->builder->CreateZExt(result, targetType);
                }
            } else {
                auto con = &recordType->cons[index - 1];
                auto conGen = (ConGen*)con->codegen;

                if(recordGen->indirect) {
                    if(recordType->conCount == 1) {
                        if(isIndirect(con->content)) {
                            // Do nothing - the result is a pointer to the correct type already.
                        } else {
                            result = gen->builder->CreateLoad(result);
                        }
                    } else {
                        auto p = gen->builder->CreateBitCast(result, llvm::PointerType::get(conGen->regType, 0));

                        llvm::Value* indices[2];
                        indices[0] = gen->builder->getInt32(0);
                        indices[1] = gen->builder->getInt32(1);
                        auto e = gen->builder->CreateGEP(p, {indices, 2});

                        if(isIndirect(con->content)) {
                            result = e;
                        } else {
                            result = gen->builder->CreateLoad(e);
                        }
                    }
                } else if(isIndirect(con->content)) {
                    auto p = gen->builder->CreateAlloca(useType(gen, con->content));

                    llvm::Value* indices[2];
                    indices[0] = gen->builder->getInt32(0);
                    indices[1] = gen->builder->getInt32(recordType->conCount == 1 ? 0 : 1);
                    auto e = gen->builder->CreateGEP(result, {indices, 2});

                    auto v = gen->builder->CreateLoad(e);
                    gen->builder->CreateStore(v, p);
                    result = p;
                } else {
                    // Do nothing - result either already has the correct type, or this case is not supported.
                }

                from = canonicalType(con->content);
            }
        } else {
            // Tuple type: simply return the field at the provided index.
            currentChain[chainLength] = inst->indexChain[i];
            chainLength++;
            from = canonicalType(((TupType*)from)->fields[index].type);
        }
    }

    if(chainLength) {
        return buildChain(gen, result, currentChain, chainLength);
    } else {
        return result;
    }
}

llvm::Value* genStringLength(Gen* gen, InstStringLength* inst) {
    llvm::Value* indices[2];
    indices[0] = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*gen->llvm), 0);
    indices[1] = indices[0];
    auto p = gen->builder->CreateGEP(useValue(gen, inst->from), {indices, 2});
    return gen->builder->CreateLoad(p);
}

llvm::Value* genStringData(Gen* gen, InstStringData* inst) {
    llvm::Value* indices[3];
    indices[0] = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*gen->llvm), 0);
    indices[1] = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*gen->llvm), 1);
    indices[2] = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*gen->llvm), 0);
    return gen->builder->CreateGEP(useValue(gen, inst->from), {indices, 3});
}

llvm::Value* genCall(Gen* gen, InstCall* inst) {
    auto args = (llvm::Value**)alloca(sizeof(llvm::Value*) * inst->argCount);
    for(U32 i = 0; i < inst->argCount; i++) {
        args[i] = useValue(gen, inst->args[i]);
    }

    // Indirect values are handled automatically - they are a pointers inside the function,
    // and they are pointers in the argument type.
    auto call = gen->builder->CreateCall(useFunction(gen, inst->fun), {args, inst->argCount});
    call->setCallingConv(llvm::CallingConv::Fast);
    return call;
}

llvm::Value* genSysCall(Gen* gen, InstCallDyn* inst) {
    llvm::Value* args[7];
    llvm::Type* argTypes[7];

    argTypes[0] = gen->builder->getInt32Ty();
    args[0] = useValue(gen, inst->fun);

    char constraints[1024];
    auto p = copyString("={rax},{rax},", constraints, 1024);

    auto argCount = inst->argCount;
    for(U32 i = 0; i < argCount; i++) {
        argTypes[i + 1] = useType(gen, inst->args[i]->type);
        args[i + 1] = useValue(gen, inst->args[i]);
    }

    if(argCount >= 1) {
        p = copyString("{rdi},", p, (U32)(1024 - (p - constraints)));
        if(argCount >= 2) {
            p = copyString("{rsi},", p, (U32)(1024 - (p - constraints)));
            if(argCount >= 3) {
                p = copyString("{rdx},", p, (U32)(1024 - (p - constraints)));
                if(argCount >= 4) {
                    p = copyString("{r10},", p, (U32)(1024 - (p - constraints)));
                    if(argCount >= 5) {
                        p = copyString("{r8},", p, (U32)(1024 - (p - constraints)));
                        if(argCount >= 6) {
                            p = copyString("{r9},", p, (U32)(1024 - (p - constraints)));
                            if(argCount > 6) {
                                gen->context->diagnostics.error("codegen: unsupported syscall argument count"_buffer, nullptr, noSource);
                            }
                        }
                    }
                }
            }
        }
    }

    p = copyString("~{dirflag},~{fpsr},~{flags},~{rcx},~{r11}", p, (U32)(1024 - (p - constraints)));

    auto funType = llvm::FunctionType::get(gen->builder->getInt8PtrTy(0), {argTypes, argCount + 1}, false);
    auto assembly = llvm::InlineAsm::get(funType, "syscall", {constraints, (U32)(p - constraints)}, true);
    return gen->builder->CreateCall(assembly, {args, argCount + 1});
}

llvm::Value* genCallIntrinsic(Gen* gen, InstCallDyn* inst) {
    auto funType = canonicalType(inst->fun->type);
    if(funType->kind == Type::Int) {
        return genSysCall(gen, inst);
    } else if(funType->kind == Type::String) {
        // TODO: Implement LLVM intrinsics.
        return nullptr;
    } else {
        // Unsupported type.
        gen->context->diagnostics.error("codegen: unsupported intrinsic call type"_buffer, nullptr, noSource);
        return nullptr;
    }
}

llvm::Value* genCallDyn(Gen* gen, InstCallDyn* inst) {
    if(inst->isIntrinsic) {
        return genCallIntrinsic(gen, inst);
    }

    return nullptr;
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
    // TODO: Handle returning indirect values.
    auto value = inst->value ? useValue(gen, inst->value) : nullptr;
    return gen->builder->CreateRet(value);
}

llvm::Value* genPhi(Gen* gen, InstPhi* inst) {
    auto type = useType(gen, inst->type);
    if(isIndirect(inst->type)) {
        type = llvm::PointerType::get(type, 0);
    }

    auto phi = gen->builder->CreatePHI(type, (U32)inst->altCount);
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
        case Inst::InstRecord:
            return genRecord(gen, (InstRecord*)inst);
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
        case Inst::InstStringLength:
            return genStringLength(gen, (InstStringLength*)inst);
        case Inst::InstStringData:
            return genStringData(gen, (InstStringData*)inst);
        case Inst::InstCall:
            return genCall(gen, (InstCall*)inst);
        case Inst::InstCallDyn:
            return genCallDyn(gen, (InstCallDyn*)inst);
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
        auto t = useType(gen, fun->args[i]->type);
        if(isIndirect(fun->args[i]->type)) {
            args[i] = llvm::PointerType::get(t, 0);
        } else {
            args[i] = t;
        }
    }

    auto sig = llvm::FunctionType::get(ret, llvm::ArrayRef<llvm::Type*>(args, argCount), false);
    auto linkage = llvm::Function::ExternalLinkage;
    auto f = llvm::Function::Create(sig, linkage, toRef(gen->context, fun->name), gen->module);

    f->setCallingConv(llvm::CallingConv::Fast);
    fun->codegen = f;

    U32 i = 0;
    for(auto it = f->arg_begin(); it != f->arg_end(); i++, it++) {
        auto arg = fun->args[i];
        it->setName(toRef(gen->context, arg->name));
        arg->codegen = &*it;
    }

    genBlock(gen, fun->blocks[0]);
    return f;
}

llvm::Value* genGlobal(Gen* gen, Global* global) {
    auto name = toRef(gen->context, global->name);
    auto type = useType(gen, ((RefType*)global->type)->to);
    auto initializer = llvm::ConstantAggregateZero::get(type);
    auto g = new llvm::GlobalVariable(*gen->module, type, false, llvm::GlobalVariable::InternalLinkage, initializer,
                                      name, nullptr, llvm::GlobalVariable::NotThreadLocal, 0, false);
    global->codegen = g;
    return g;
}

llvm::Module* genModule(llvm::LLVMContext* llvm, Context* context, Module* module) {
    auto moduleName = &context->find(module->id);

    auto name = llvm::StringRef{moduleName->text, moduleName->textLength};
    auto llvmModule = new llvm::Module(name, *llvm);
    llvmModule->setDataLayout("e-S128");
    llvmModule->setTargetTriple(LLVM_HOST_TRIPLE);

    llvm::IRBuilder<> builder(*llvm);

    LLVMTypes types;
    types.stringData = genStringType(llvm);
    types.stringRef = llvm::PointerType::get(types.stringData, 0);

    Gen gen{llvm, llvmModule, &builder, types, &context->exprArena, context};

    for(auto& global: module->globals) {
        genGlobal(&gen, &global);
    }

    for(auto& fun: module->functions) {
        // Functions can be lazily generated depending on their usage order, so we only generate if needed.
        useFunction(&gen, &fun);
    }

    for(const InstanceMap& map: module->classInstances) {
        for(U32 i = 0; i < map.instances.size(); i++) {
            ClassInstance* instance = map.instances[i];
            auto instances = instance->instances;
            auto funCount = instance->typeClass->funCount;

            for(U32 j = 0; j < funCount; j++) {
                useFunction(&gen, instances[j]);
            }
        }
    }

    return llvmModule;
}