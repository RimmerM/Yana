#include "inst.h"
#include "block.h"
#include "module.h"
#include <initializer_list>

void setArg(Inst* inst, Value** args, Size index, Value* arg) {
    args[index] = arg;
    inst->usedValues[index] = arg;
    inst->block->use(arg, inst);
}

static void useValues(Inst* inst, Block* block, std::initializer_list<Value*> values) {
    auto v = (Value**)block->function->module->memory.alloc(sizeof(Value*) * values.size());
    inst->usedValues = v;
    inst->usedCount = values.size();

    for(auto it: values) {
        *v++ = it;
        block->use(it, inst);
    }
}

static InstCast* cast(Block* block, Inst::Kind kind, Id name, Value* from, Type* to) {
    auto inst = (InstCast*)block->inst(sizeof(InstCast), name, kind, to);
    inst->from = from;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

static InstBinary* binary(Block* block, Inst::Kind kind, Id name, Value* lhs, Value* rhs, Type* to) {
    auto inst = (InstBinary*)block->inst(sizeof(InstBinary), name, kind, to);
    inst->lhs = lhs;
    inst->rhs = rhs;

    inst->usedValues = &inst->lhs;
    inst->usedCount = 2;
    block->use(lhs, inst);
    block->use(rhs, inst);

    return inst;
}

ConstInt* constInt(Block* block, I64 value) {
    auto c = (ConstInt*)block->function->module->memory.alloc(sizeof(ConstInt));
    c->block = block;
    c->name = 0;
    c->kind = Value::ConstInt;
    c->type = &intTypes[IntType::Int];
    return c;
}

ConstFloat* constFloat(Block* block, double value) {
    auto c = (ConstFloat*)block->function->module->memory.alloc(sizeof(ConstFloat));
    c->block = block;
    c->name = 0;
    c->kind = Value::ConstFloat;
    c->type = &floatTypes[FloatType::F64];
    return c;
}

ConstString* constString(Block* block, const char* value, Size length) {
    auto c = (ConstString*)block->function->module->memory.alloc(sizeof(ConstString));
    c->block = block;
    c->name = 0;
    c->kind = Value::ConstString;
    c->type = &stringType;
    return c;
}

InstTrunc* trunc(Block* block, Id name, Value* from, Type* to) {
    return (InstTrunc*)cast(block, Inst::InstTrunc, name, from, to);
}

InstFTrunc* ftrunc(Block* block, Id name, Value* from, Type* to) {
    return (InstFTrunc*)cast(block, Inst::InstFTrunc, name, from, to);
}

InstZExt* zext(Block* block, Id name, Value* from, Type* to) {
    return (InstZExt*)cast(block, Inst::InstZExt, name, from, to);
}

InstSExt* sext(Block* block, Id name, Value* from, Type* to) {
    return (InstSExt*)cast(block, Inst::InstSExt, name, from, to);
}

InstFExt* fext(Block* block, Id name, Value* from, Type* to) {
    return (InstFExt*)cast(block, Inst::InstFExt, name, from, to);
}

InstAdd* add(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstAdd*)binary(block, Inst::InstAdd, name, lhs, rhs, lhs->type);
}

InstSub* sub(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstSub*)binary(block, Inst::InstSub, name, lhs, rhs, lhs->type);
}

InstMul* mul(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstMul*)binary(block, Inst::InstMul, name, lhs, rhs, lhs->type);
}

InstDiv* div(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstDiv*)binary(block, Inst::InstDiv, name, lhs, rhs, lhs->type);
}

InstIDiv* idiv(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstIDiv*)binary(block, Inst::InstIDiv, name, lhs, rhs, lhs->type);
}

InstRem* rem(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstRem*)binary(block, Inst::InstRem, name, lhs, rhs, lhs->type);
}

InstIRem* irem(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstIRem*)binary(block, Inst::InstIRem, name, lhs, rhs, lhs->type);
}

InstFAdd* fadd(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstFAdd*)binary(block, Inst::InstFAdd, name, lhs, rhs, lhs->type);
}

InstFSub* fsub(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstFSub*)binary(block, Inst::InstFSub, name, lhs, rhs, lhs->type);
}

InstFMul* fmul(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstFMul*)binary(block, Inst::InstFMul, name, lhs, rhs, lhs->type);
}

InstFDiv* fdiv(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstFDiv*)binary(block, Inst::InstFDiv, name, lhs, rhs, lhs->type);
}

InstICmp* icmp(Block* block, Id name, Value* lhs, Value* rhs, ICmp cmp) {
    auto inst = (InstICmp*)block->inst(sizeof(InstICmp), name, Inst::InstICmp, &intTypes[IntType::Bool]);
    inst->lhs = lhs;
    inst->rhs = rhs;
    inst->cmp = cmp;

    inst->usedValues = &inst->lhs;
    inst->usedCount = 2;
    block->use(lhs, inst);
    block->use(rhs, inst);

    return inst;
}

InstFCmp* fcmp(Block* block, Id name, Value* lhs, Value* rhs, FCmp cmp) {
    auto inst = (InstFCmp*)block->inst(sizeof(InstFCmp), name, Inst::InstFCmp, &intTypes[IntType::Bool]);
    inst->lhs = lhs;
    inst->rhs = rhs;
    inst->cmp = cmp;

    inst->usedValues = &inst->lhs;
    inst->usedCount = 2;
    block->use(lhs, inst);
    block->use(rhs, inst);

    return inst;
}

InstShl* shl(Block* block, Id name, Value* arg, Value* amount) {
    return (InstShl*)binary(block, Inst::InstShl, name, arg, amount, arg->type);
}

InstShr* shr(Block* block, Id name, Value* arg, Value* amount) {
    return (InstShr*)binary(block, Inst::InstShr, name, arg, amount, arg->type);
}

InstSar* sar(Block* block, Id name, Value* arg, Value* amount) {
    return (InstSar*)binary(block, Inst::InstSar, name, arg, amount, arg->type);
}

InstAnd* and_(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstAnd*)binary(block, Inst::InstAnd, name, lhs, rhs, lhs->type);
}

InstOr* or_(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstOr*)binary(block, Inst::InstOr, name, lhs, rhs, lhs->type);
}

InstXor* xor_(Block* block, Id name, Value* lhs, Value* rhs) {
    return (InstXor*)binary(block, Inst::InstXor, name, lhs, rhs, lhs->type);
}

InstRecord* record(Block* block, Id name, struct Con* con, Value* arg) {
    auto inst = (InstRecord*)block->inst(sizeof(InstRecord), 0, Inst::InstRecord, con->parent);
    inst->con = con;
    inst->arg = arg;

    inst->usedValues = &inst->arg;
    inst->usedCount = 1;
    block->use(arg, inst);

    return inst;
}

InstTup* tup(Block* block, Id name, Type* type, Size fieldCount) {
    auto inst = (InstTup*)block->inst(sizeof(InstTup), name, Inst::InstTup, type);
    auto fields = (Value**)block->function->module->memory.alloc(sizeof(Value*) * fieldCount);
    inst->fields = fields;
    inst->fieldCount = fieldCount;

    inst->usedValues = fields;
    inst->usedCount = fieldCount;

    return inst;
}

InstFun* fun(Block* block, Id name, struct Function* body, Type* type, Size frameCount) {
    auto inst = (InstFun*)block->inst(sizeof(InstFun), name, Inst::InstFun, type);
    auto frame = (Value**)block->function->module->memory.alloc(sizeof(Value*) * frameCount);
    inst->body = body;
    inst->frame = frame;
    inst->frameCount = frameCount;

    inst->usedValues = frame;
    inst->usedCount = frameCount;

    return inst;
}

InstAlloc* alloc(Block* block, Id name, Type* type, bool mut) {
    auto inst = (InstAlloc*)block->inst(sizeof(InstAlloc), name, Inst::InstAlloc, getRef(block->function->module, type));
    inst->valueType = type;
    inst->mut = mut;
    return inst;
}

InstLoad* load(Block* block, Id name, Value* from) {
    assert(from->type->kind == Type::Ref || from->type->kind == Type::Ptr);
    auto inst = (InstLoad*)block->inst(sizeof(InstLoad), name, Inst::InstLoad, ((RefType*)from->type)->to);
    inst->from = from;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

InstLoadField* loadField(Block* block, Id name, Value* from, U32* indices, U32 count) {
    assert(from->type->kind == Type::Ref || from->type->kind == Type::Ptr);
    auto inst = (InstLoadField*)block->inst(sizeof(InstLoadField), name, Inst::InstLoadField, ((RefType*)from->type)->to);
    inst->from = from;
    inst->indexChain = indices;
    inst->chainLength = count;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

InstStore* store(Block* block, Id name, Value* to, Value* value) {
    assert(to->type->kind == Type::Ref || to->type->kind == Type::Ptr);
    auto inst = (InstStore*)block->inst(sizeof(InstStore), name, Inst::InstStore, ((RefType*)to->type)->to);
    inst->to = to;
    inst->value = value;

    inst->usedValues = &inst->to;
    inst->usedCount = 2;
    block->use(to, inst);
    block->use(value, inst);

    return inst;
}

InstStoreField* storeField(Block* block, Id name, Value* to, Value* value, U32* indices, U32 count) {
    assert(to->type->kind == Type::Ref || to->type->kind == Type::Ptr);
    auto inst = (InstStoreField*)block->inst(sizeof(InstStoreField), name, Inst::InstStoreField, ((RefType*)to->type)->to);
    inst->to = to;
    inst->value = value;
    inst->indexChain = indices;
    inst->chainLength = count;

    inst->usedValues = &inst->to;
    inst->usedCount = 2;
    block->use(to, inst);
    block->use(value, inst);

    return inst;
}

InstGetField* getField(Block* block, Id name, Value* from, Type* type, U32* indices, U32 count) {
    auto inst = (InstGetField*)block->inst(sizeof(InstGetField), name, Inst::InstGetField, type);
    inst->from = from;
    inst->indexChain = indices;
    inst->chainLength = count;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

InstUpdateField* updateField(Block* block, Id name, Value* from, InstUpdateField::Field* fields, U32 count) {
    auto inst = (InstUpdateField*)block->inst(sizeof(InstUpdateField), name, Inst::InstUpdateField, from->type);
    inst->from = from;
    inst->fields = fields;
    inst->fieldCount = count;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    auto v = (Value**)block->function->module->memory.alloc(sizeof(Value*) * (count + 1));
    inst->usedValues = v;
    inst->usedCount = count + 1;

    *v = from;
    block->use(from, inst);
    v++;

    for(U32 i = 0; i < count; i++) {
        *v++ = fields[i].value;
        block->use(fields[i].value, inst);
    }

    return inst;
}

InstCall* call(Block* block, Id name, struct Function* fun, Size argCount) {
    auto inst = (InstCall*)block->inst(sizeof(InstCall), name, Inst::InstCall, fun->returnType);
    auto args = (Value**)block->function->module->memory.alloc(sizeof(Value*) * argCount);
    inst->fun = fun;
    inst->args = args;
    inst->argCount = argCount;

    inst->usedValues = args;
    inst->usedCount = argCount;

    return inst;
}

InstCallGen* callGen(Block* block, Id name, struct Function* fun, Size argCount) {
    auto inst = call(block, name, fun, argCount);
    inst->kind = Inst::InstCallDyn;
    return (InstCallGen*)inst;
}

InstCallDyn* callDyn(Block* block, Id name, Value* fun, Size argCount) {
    auto type = (FunType*)fun->type;
    auto inst = (InstCallDyn*)block->inst(sizeof(InstCallDyn), name, Inst::InstCallDyn, type->result);
    auto args = (Value**)block->function->module->memory.alloc(sizeof(Value*) * (argCount + 1));
    inst->fun = fun;
    inst->args = args;
    inst->argCount = argCount;

    inst->usedValues = args;
    inst->usedCount = argCount + 1;
    inst->usedValues[argCount] = fun;
    block->use(fun, inst);

    return inst;
}

InstCallDynGen* callDynGen(Block* block, Id name, Value* fun, Size argCount) {
    auto inst = callDyn(block, name, fun, argCount);
    inst->kind = Inst::InstCallDynGen;
    return (InstCallDynGen*)inst;
}

InstCallForeign* callForeign(Block* block, Id name, struct ForeignFunction* fun, Size argCount) {
    auto inst = (InstCallForeign*)block->inst(sizeof(InstCallForeign), name, Inst::InstCallForeign, fun->type->result);
    auto args = (Value**)block->function->module->memory.alloc(sizeof(Value*) * argCount);
    inst->fun = fun;
    inst->args = args;
    inst->argCount = argCount;

    inst->usedValues = args;
    inst->usedCount = argCount;

    return inst;
}

InstJe* je(Block* block, Value* cond, Block* then, Block* otherwise) {
    auto inst = (InstJe*)block->inst(sizeof(InstJe), 0, Inst::InstJe, &unitType);
    inst->cond = cond;
    inst->then = then;
    inst->otherwise = otherwise;

    inst->usedValues = &inst->cond;
    inst->usedCount = 1;
    block->use(cond, inst);

    useValues(inst, block, {cond});

    block->outgoing.push(then);
    block->outgoing.push(otherwise);
    then->incoming.push(block);
    otherwise->incoming.push(block);

    return inst;
}

InstJmp* jmp(Block* block, Block* to) {
    auto inst = (InstJmp*)block->inst(sizeof(InstJmp), 0, Inst::InstJmp, &unitType);
    inst->to = to;
    block->outgoing.push(to);
    to->incoming.push(block);

    return inst;
}

InstRet* ret(Block* block, Value* value) {
    // Use the type of the returned value to simplify some analysis.
    auto type = value ? value->type : &unitType;
    auto inst = (InstRet*)block->inst(sizeof(InstRet), 0, Inst::InstRet, type);

    inst->value = value;
    if(value) {
        inst->usedValues = &inst->value;
        inst->usedCount = 1;
        block->use(value, inst);
    }

    block->returns = true;
    block->function->returnPoints.push(inst);

    return inst;
}

InstPhi* phi(Block* block, Id name, InstPhi::Alt* alts, Size altCount) {
    auto inst = (InstPhi*)block->inst(sizeof(InstPhi), name, Inst::InstPhi, alts[0].value->type);
    inst->alts = alts;
    inst->altCount = altCount;

    auto v = (Value**)block->function->module->memory.alloc(sizeof(Value*) * altCount);
    inst->usedValues = v;
    inst->usedCount = altCount;

    for(Size i = 0; i < altCount; i++) {
        *v++ = alts[i].value;
        block->use(alts[i].value, inst);
    }

    return inst;
}
