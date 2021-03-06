#include "inst.h"
#include "block.h"
#include "module.h"
#include <initializer_list>

static void useValues(Inst* inst, Block* block, std::initializer_list<Value*> values) {
    auto v = (Value**)block->function->module->memory.alloc(sizeof(Value*) * values.size());
    inst->usedValues = v;
    inst->usedCount = values.size();

    for(auto it: values) {
        *v++ = it;
        block->use(it, inst);
    }
}

static bool isConstant(Value* v) {
    return v->kind >= Value::FirstConst && v->kind <= Value::LastConst;
}

template<class F>
static Value* constantFoldInt(Block* block, Id name, Value* lhs, Value* rhs, F&& f) {
    if(isConstant(lhs) && isConstant(rhs)) {
        auto result = f(((ConstInt*)lhs)->value, ((ConstInt*)rhs)->value);
        return constInt(block, name, result, lhs->type);
    } else {
        return nullptr;
    }
}

template<class F>
static Value* constantFoldFloat(Block* block, Id name, Value* lhs, Value* rhs, F&& f) {
    if(isConstant(lhs) && isConstant(rhs)) {
        auto result = f(((ConstFloat*)lhs)->value, ((ConstFloat*)rhs)->value);
        return constFloat(block, name, result, lhs->type);
    } else {
        return nullptr;
    }
}

template<class T, class F>
static Value* constantFoldCmp(Block* block, Id name, Value* lhs, Value* rhs, F&& f) {
    if(isConstant(lhs) && isConstant(rhs)) {
        auto result = f(((T*)lhs)->value, ((T*)rhs)->value);
        return constInt(block, name, result, &intTypes[IntType::Bool]);
    } else {
        return nullptr;
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

Value* error(Block* block, Id name, Type* type) {
    auto v = block->inst(sizeof(Inst), name, Value::InstNop, type);
    v->usedCount = 0;
    v->usedValues = nullptr;
    return v;
}

Value* nop(Block* block, Id name) {
    auto v = block->inst(sizeof(Inst), name, Value::InstNop, &unitType);
    v->usedCount = 0;
    v->usedValues = nullptr;
    return v;
}

ConstInt* constInt(Block* block, Id name, U64 value, Type* type) {
    auto c = new (block->function->module->memory) ConstInt;
    c->block = block;
    c->name = name;
    c->kind = Value::ConstInt;
    c->type = type;
    c->value = value;

    if(name) {
        block->namedValues[name] = c;
    }
    return c;
}

ConstFloat* constFloat(Block* block, Id name, double value, Type* type) {
    auto c = new (block->function->module->memory) ConstFloat;
    c->block = block;
    c->name = name;
    c->kind = Value::ConstFloat;
    c->type = type;
    c->value = value;

    if(name) {
        block->namedValues[name] = c;
    }
    return c;
}

ConstString* constString(Block* block, Id name, const char* value, Size length) {
    auto c = new (block->function->module->memory) ConstString;
    c->block = block;
    c->name = name;
    c->kind = Value::ConstString;
    c->type = &stringType;
    c->value = value;
    c->length = length;

    if(name) {
        block->namedValues[name] = c;
    }
    return c;
}

Value* trunc(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstInt) {
        auto value = ((ConstInt*)from)->value;
        auto toType = (IntType*)to;

        switch(toType->width) {
            case IntType::Bool:
                return constInt(block, name, value != 0 ? 1 : 0, to);
            case IntType::Int:
                return constInt(block, name, (U32)value, to);
            case IntType::Long:
                return constInt(block, name, (U64)value, to);
        }
    }

    return cast(block, Inst::InstTrunc, name, from, to);
}

Value* ftrunc(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstFloat) {
        auto value = ((ConstFloat*)from)->value;
        auto toType = (FloatType*)to;

        if(toType->width == FloatType::F64) {
            return constFloat(block, name, (double)value, to);
        } else if(toType->width == FloatType::F32) {
            return constFloat(block, name, (float)value, to);
        }

        // TODO: Support constant folding for remaining floating point types.
    }

    return cast(block, Inst::InstFTrunc, name, from, to);
}

Value* zext(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstInt) {
        return constInt(block, name, ((ConstInt*)from)->value, to);
    } else {
        return cast(block, Inst::InstZExt, name, from, to);
    }
}

Value* sext(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstInt) {
        return constInt(block, name, ((ConstInt*)from)->value, to);
    } else {
        return cast(block, Inst::InstSExt, name, from, to);
    }
}

Value* fext(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstFloat) {
        return constFloat(block, name, ((ConstFloat*)from)->value, to);
    } else {
        return cast(block, Inst::InstFExt, name, from, to);
    }
}

Value* itof(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstInt) {
        return constFloat(block, name, (double)((I64)((ConstInt*)from)->value), to);
    } else {
        return cast(block, Inst::InstIToF, name, from, to);
    }
}

Value* uitof(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstInt) {
        return constFloat(block, name, (double)((U64)((ConstInt*)from)->value), to);
    } else {
        return cast(block, Inst::InstUIToF, name, from, to);
    }
}

Value* ftoi(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstFloat) {
        return constInt(block, name, (U64)(I64)((ConstFloat*)from)->value, to);
    } else {
        return cast(block, Inst::InstFToI, name, from, to);
    }
}

Value* ftoui(Block* block, Id name, Value* from, Type* to) {
    if(from->kind == Value::ConstFloat) {
        return constInt(block, name, (U64)((ConstFloat*)from)->value, to);
    } else {
        return cast(block, Inst::InstFToUI, name, from, to);
    }
}

Value* add(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return a + b; })) return v;
    return binary(block, Inst::InstAdd, name, lhs, rhs, lhs->type);
}

Value* sub(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return a - b; })) return v;
    return binary(block, Inst::InstSub, name, lhs, rhs, lhs->type);
}

Value* mul(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return a * b; })) return v;
    return binary(block, Inst::InstMul, name, lhs, rhs, lhs->type);
}

Value* div(Block* block, Id name, Value* lhs, Value* rhs) {
    if(rhs->kind == Value::ConstInt && ((ConstInt*)rhs)->value != 0) {
        if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return U64(a) / U64(b); })) return v;
    }
    return binary(block, Inst::InstDiv, name, lhs, rhs, lhs->type);
}

Value* idiv(Block* block, Id name, Value* lhs, Value* rhs) {
    if(rhs->kind == Value::ConstInt && ((ConstInt*)rhs)->value != 0) {
        if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return a / b; })) return v;
    }
    return binary(block, Inst::InstIDiv, name, lhs, rhs, lhs->type);
}

Value* rem(Block* block, Id name, Value* lhs, Value* rhs) {
    if(rhs->kind == Value::ConstInt && ((ConstInt*)rhs)->value != 0) {
        if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return U64(a) % U64(b); })) return v;
    }
    return binary(block, Inst::InstRem, name, lhs, rhs, lhs->type);
}

Value* irem(Block* block, Id name, Value* lhs, Value* rhs) {
    if(rhs->kind == Value::ConstInt && ((ConstInt*)rhs)->value != 0) {
        if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return a % b; })) return v;
    }
    return binary(block, Inst::InstIRem, name, lhs, rhs, lhs->type);
}

Value* fadd(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldFloat(block, name, lhs, rhs, [=](auto a, auto b) { return a + b; })) return v;
    return binary(block, Inst::InstFAdd, name, lhs, rhs, lhs->type);
}

Value* fsub(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldFloat(block, name, lhs, rhs, [=](auto a, auto b) { return a - b; })) return v;
    return binary(block, Inst::InstFSub, name, lhs, rhs, lhs->type);
}

Value* fmul(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldFloat(block, name, lhs, rhs, [=](auto a, auto b) { return a * b; })) return v;
    return binary(block, Inst::InstFMul, name, lhs, rhs, lhs->type);
}

Value* fdiv(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldFloat(block, name, lhs, rhs, [=](auto a, auto b) { return a / b; })) return v;
    return binary(block, Inst::InstFDiv, name, lhs, rhs, lhs->type);
}

Value* icmp(Block* block, Id name, Value* lhs, Value* rhs, ICmp cmp) {
    if(auto v = constantFoldCmp<ConstInt>(block, name, lhs, rhs, [=](auto a, auto b) {
        switch(cmp) {
            case ICmp::eq: return a == b;
            case ICmp::neq: return a != b;
            case ICmp::gt: return (U64)a > (U64)b;
            case ICmp::ge: return (U64)a >= (U64)b;
            case ICmp::lt: return (U64)a < (U64)b;
            case ICmp::le: return (U64)a <= (U64)b;
            case ICmp::igt: return a > b;
            case ICmp::ige: return a >= b;
            case ICmp::ilt: return a < b;
            case ICmp::ile: return a <= b;
        }
    })) return v;

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

Value* fcmp(Block* block, Id name, Value* lhs, Value* rhs, FCmp cmp) {
    if(auto v = constantFoldCmp<ConstFloat>(block, name, lhs, rhs, [=](auto a, auto b) {
        switch(cmp) {
            case FCmp::eq: return a == b;
            case FCmp::neq: return a != b;
            case FCmp::gt: return a > b;
            case FCmp::ge: return a >= b;
            case FCmp::lt: return a < b;
            case FCmp::le: return a <= b;
        }
    })) return v;

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

Value* shl(Block* block, Id name, Value* arg, Value* amount) {
    if(auto v = constantFoldInt(block, name, arg, amount, [=](auto a, auto b) { return a << b; })) return v;
    return binary(block, Inst::InstShl, name, arg, amount, arg->type);
}

Value* shr(Block* block, Id name, Value* arg, Value* amount) {
    if(auto v = constantFoldInt(block, name, arg, amount, [=](auto a, auto b) { return (U64)a >> b; })) return v;
    return binary(block, Inst::InstShr, name, arg, amount, arg->type);
}

Value* sar(Block* block, Id name, Value* arg, Value* amount) {
    if(auto v = constantFoldInt(block, name, arg, amount, [=](auto a, auto b) { return a >> b; })) return v;
    return binary(block, Inst::InstSar, name, arg, amount, arg->type);
}

Value* and_(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return a & b; })) return v;
    return binary(block, Inst::InstAnd, name, lhs, rhs, lhs->type);
}

Value* or_(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return a | b; })) return v;
    return binary(block, Inst::InstOr, name, lhs, rhs, lhs->type);
}

Value* xor_(Block* block, Id name, Value* lhs, Value* rhs) {
    if(auto v = constantFoldInt(block, name, lhs, rhs, [=](auto a, auto b) { return a ^ b; })) return v;
    return binary(block, Inst::InstXor, name, lhs, rhs, lhs->type);
}

Value* addref(Block* block, Id name, Value* lhs, Value* rhs) {
    return binary(block, Inst::InstAddRef, name, lhs, rhs, lhs->type);
}

InstRecord* record(Block* block, Id name, struct Con* con, Value* content) {
    auto inst = (InstRecord*)block->inst(sizeof(InstRecord), name, Inst::InstRecord, con->parent);

    inst->con = con;
    inst->content = content;

    if(content) {
        inst->usedValues = &inst->content;
        inst->usedCount = 1;
        inst->block->use(content, inst);
    } else {
        inst->usedCount = 0;
    }

    return inst;
}

InstTup* tup(Block* block, Id name, Type* type, Value** fields, U32 count) {
    auto inst = (InstTup*)block->inst(sizeof(InstTup), name, Inst::InstTup, type);

    inst->fields = fields;
    inst->fieldCount = count;

    inst->usedValues = fields;
    inst->usedCount = count;

    for(U32 i = 0; i < count; i++) {
        inst->block->use(fields[i], inst);
    }

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

InstAlloc* alloc(Block* block, Id name, Type* type, bool mut, bool local) {
    auto refType = getRef(block->function->module, type, !local, local, mut);
    auto inst = (InstAlloc*)block->inst(sizeof(InstAlloc), name, Inst::InstAlloc, refType);

    inst->valueType = type;
    inst->mut = mut;
    return inst;
}

InstAllocArray* allocArray(Block* block, Id name, Type* type, Value* length, bool mut, bool local) {
    auto module = block->function->module;
    auto arrayType = getArray(module, type);
    auto inst = (InstAllocArray*)block->inst(sizeof(InstAllocArray), name, Inst::InstAllocArray, arrayType);

    inst->length = length;
    inst->valueType = type;
    inst->mut = mut;

    inst->usedValues = &inst->length;
    inst->usedCount = 1;
    block->use(length, inst);

    return inst;
}

InstLoad* load(Block* block, Id name, Value* from) {
    assertTrue(from->type->kind == Type::Ref);
    auto inst = (InstLoad*)block->inst(sizeof(InstLoad), name, Inst::InstLoad, ((RefType*)from->type)->to);
    inst->from = from;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

InstLoadField* loadField(Block* block, Id name, Value* from, Type* type, U32* indices, U32 count) {
    assertTrue(from->type->kind == Type::Ref);
    auto inst = (InstLoadField*)block->inst(sizeof(InstLoadField), name, Inst::InstLoadField, type);
    inst->from = from;
    inst->indexChain = indices;
    inst->chainLength = count;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

InstLoadArray* loadArray(Block* block, Id name, Value* from, Value* index, Type* type, bool checked) {
    auto inst = (InstLoadArray*)block->inst(sizeof(InstLoadArray), name, Inst::InstLoadArray, type);
    inst->from = from;
    inst->index = index;
    inst->checked = checked;

    inst->usedValues = &inst->from;
    inst->usedCount = 2;
    block->use(from, inst);
    block->use(index, inst);

    return inst;
}

InstStore* store(Block* block, Id name, Value* to, Value* value) {
    assertTrue(to->type->kind == Type::Ref);
    auto inst = (InstStore*)block->inst(sizeof(InstStore), name, Inst::InstStore, &unitType);
    inst->to = to;
    inst->value = value;

    inst->usedValues = &inst->to;
    inst->usedCount = 2;
    block->use(to, inst);
    block->use(value, inst);

    return inst;
}

InstStoreField* storeField(Block* block, Id name, Value* to, Value* value, U32* indices, U32 count) {
    assertTrue(to->type->kind == Type::Ref);
    auto inst = (InstStoreField*)block->inst(sizeof(InstStoreField), name, Inst::InstStoreField, &unitType);
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

InstStoreArray* storeArray(Block* block, Id name, Value* to, Value* index, Value** values, U32 count, bool checked) {
    auto inst = (InstStoreArray*)block->inst(sizeof(InstStoreArray), name, Inst::InstStoreArray, &unitType);
    inst->to = to;
    inst->index = index;
    inst->values = values;
    inst->count = count;
    inst->checked = checked;

    auto v = (Value**)block->function->module->memory.alloc(sizeof(Value*) * (count + 2));
    inst->usedValues = v;
    inst->usedCount = 2 + count;

    *v++ = to;
    block->use(to, inst);

    *v++ = index;
    block->use(index, inst);

    for(U32 i = 0; i < count; i++) {
        *v++ = values[i];
        block->use(values[i], inst);
    }

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

InstArrayLength* arrayLength(Block* block, Id name, Value* from) {
    auto inst = (InstArrayLength*)block->inst(sizeof(InstArrayLength), name, Inst::InstArrayLength, &intTypes[IntType::Int]);
    inst->from = from;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

InstArrayCopy* arrayCopy(Block* block, Id name, Value* from, Value* to, Value* offset, Value* count, bool checked) {
    auto inst = (InstArrayCopy*)block->inst(sizeof(InstArrayCopy), name, Inst::InstArrayCopy, from->type);
    inst->from = from;
    inst->to = to;
    inst->startIndex = offset;
    inst->count = count;
    inst->checked = checked;

    inst->usedValues = &inst->from;
    inst->usedCount = 4;
    block->use(from, inst);
    block->use(to, inst);
    block->use(offset, inst);
    block->use(count, inst);

    return inst;
}

InstArraySlice* arraySlice(Block* block, Id name, Value* from, Value* start, Value* count) {
    auto inst = (InstArraySlice*)block->inst(sizeof(InstArraySlice), name, Inst::InstArraySlice, from->type);
    inst->from = from;
    inst->startIndex = start;
    inst->count = count;

    inst->usedValues = &inst->from;
    inst->usedCount = 3;
    block->use(from, inst);
    block->use(start, inst);
    block->use(count, inst);

    return inst;
}

Value* stringLength(Block* block, Id name, Value* from) {
    auto inst = (InstStringLength*)block->inst(sizeof(InstStringLength), name, Inst::InstStringLength, &intTypes[IntType::Int]);
    inst->from = from;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

Value* stringData(Block* block, Id name, Value* from) {
    auto inst = (InstStringData*)block->inst(sizeof(InstStringData), name, Inst::InstStringData, from->type);
    inst->from = from;

    inst->usedValues = &inst->from;
    inst->usedCount = 1;
    block->use(from, inst);

    return inst;
}

InstCall* call(Block* block, Id name, struct Function* fun, Value** args, U32 count, GenInstance* instance) {
    auto inst = (InstCall*)block->inst(sizeof(InstCall), name, Inst::InstCall, fun->returnType);
    inst->fun = fun;
    inst->args = args;
    inst->argCount = count;
    inst->instance = instance;

    inst->usedValues = args;
    inst->usedCount = count;

    for(U32 i = 0; i < count; i++) {
        inst->block->use(args[i], inst);
    }

    return inst;
}

InstCallDyn* callDyn(Block* block, Id name, Value* fun, Type* type, Value** args, U32 count, GenInstance* instance, bool isIntrinsic) {
    auto inst = (InstCallDyn*)block->inst(sizeof(InstCallDyn), name, Inst::InstCallDyn, type);
    auto usedValues = (Value**)block->function->module->memory.alloc(sizeof(Value*) * (count + 1));

    inst->fun = fun;
    inst->args = args;
    inst->argCount = count;
    inst->instance = instance;
    inst->isIntrinsic = isIntrinsic;

    inst->usedValues = usedValues;
    inst->usedCount = count + 1;
    inst->usedValues[0] = fun;
    block->use(fun, inst);

    for(U32 i = 0; i < count; i++) {
        usedValues[i + 1] = args[i];
        inst->block->use(args[i], inst);
    }

    return inst;
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
    block->succeeding = to;
    to->incoming.push(block);

    return inst;
}

InstRet* ret(Block* block, Value* value) {
    // Prevent weird edge cases where we try to explicitly use a void value.
    // If the returned value is void, return nothing instead.
    if(value && value->type->kind == Type::Unit) value = nullptr;

    // Use the type of the returned value to simplify some analysis.
    auto type = value ? value->type : &unitType;
    auto inst = (InstRet*)block->inst(sizeof(InstRet), 0, Inst::InstRet, type);

    inst->value = value;
    if(value) {
        inst->usedValues = &inst->value;
        inst->usedCount = 1;
        block->use(value, inst);
    }

    block->succeeding = nullptr;
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
        auto value = alts[i].value;
        *v++ = value;

        // Don't assume that each value exists, in order to support delayed creation of alts.
        // This is needed when an alt depends on a value resolved later.
        if(value) {
            block->use(alts[i].value, inst);
        }
    }

    return inst;
}
