
#include "module.h"

typedef Value* (*BinIntrinsic)(Block*, Id, Value*, Value*);

template<BinIntrinsic F>
static Function* binaryFunction(Context* context, Module* module, Type* type, const char* name, U32 length, Type* returnType = nullptr) {
    auto fun = length ? defineFun(context, module, context->addUnqualifiedName(name, length)) : defineAnonymousFun(context, module);
    auto lhs = defineArg(context, fun, 0, type);
    auto rhs = defineArg(context, fun, 0, type);
    fun->returnType = returnType ? returnType : type;

    auto body = block(fun);
    auto result = F(body, 0, lhs, rhs);
    ret(body, result);

    fun->intrinsic = [](FunBuilder* b, Value** args, U32 count, Id instName) -> Value* {
        return F(b->block, instName, args[0], args[1]);
    };

    return fun;
}

template<class Cmp>
using CmpIntrinsic = Value* (*)(Block*, Id, Value*, Value*, Cmp);

template<class Cmp, CmpIntrinsic<Cmp> F, Cmp cmp>
static Function* cmpFunction(Context* context, Module* module, Type* type, const char* name, U32 length) {
    auto fun = length ? defineFun(context, module, context->addUnqualifiedName(name, length)) : defineAnonymousFun(context, module);
    auto lhs = defineArg(context, fun, 0, type);
    auto rhs = defineArg(context, fun, 0, type);
    fun->returnType = &intTypes[IntType::Bool];

    auto body = block(fun);
    auto result = F(body, 0, lhs, rhs, cmp);
    ret(body, result);

    fun->intrinsic = [](FunBuilder* b, Value** args, U32 count, Id instName) -> Value* {
        return F(b->block, instName, args[0], args[1], cmp);
    };

    return fun;
}

template<class Cmp, CmpIntrinsic<Cmp> cmp, Cmp Eq, Cmp Gt>
static Function* ordCompare(Context* context, Module* module, Type* type, RecordType* orderingType, const char* name, U32 length) {
    auto fun = length ? defineFun(context, module, context->addUnqualifiedName(name, length)) : defineAnonymousFun(context, module);
    auto lhs = defineArg(context, fun, 0, type);
    auto rhs = defineArg(context, fun, 0, type);
    fun->returnType = orderingType;

    auto eqTest = block(fun);
    auto eqBlock = block(fun);
    auto gtTest = block(fun);
    auto gtBlock = block(fun);
    auto ltBlock = block(fun);

    auto eq = cmp(eqTest, 0, lhs, rhs, Eq);
    je(eqTest, eq, eqBlock, gtTest);

    auto eqResult = record(eqBlock, 0, &orderingType->cons[1], nullptr);
    ret(eqBlock, eqResult);

    auto gt = cmp(gtTest, 0, lhs, rhs, Gt);
    je(gtTest, gt, gtBlock, ltBlock);

    auto gtResult = record(gtBlock, 0, &orderingType->cons[2], nullptr);
    ret(gtBlock, gtResult);

    auto ltResult = record(ltBlock, 0, &orderingType->cons[0], nullptr);
    ret(ltBlock, ltResult);

    return fun;
}

template<class Cmp, CmpIntrinsic<Cmp> cmp, Cmp Gt>
static Function* maxCompare(Context* context, Module* module, Type* type, const char* name, U32 length) {
    auto fun = length ? defineFun(context, module, context->addUnqualifiedName(name, length)) : defineAnonymousFun(context, module);
    auto lhs = defineArg(context, fun, 0, type);
    auto rhs = defineArg(context, fun, 0, type);
    fun->returnType = type;

    auto gtTest = block(fun);
    auto gtBlock = block(fun);
    auto ltBlock = block(fun);

    auto gt = cmp(gtTest, 0, lhs, rhs, Gt);
    je(gtTest, gt, gtBlock, ltBlock);
    ret(gtBlock, lhs);
    ret(ltBlock, rhs);

    return fun;
}

GenType* setClassType(Module* module, TypeClass* type, U32 funCount) {
    type->args = new (module->memory) GenType(0, 0);
    type->argCount = 1;

    type->funCount = (U16)funCount;
    type->funNames = (Id*)module->memory.alloc(sizeof(Id) * funCount);
    type->functions = (FunType*)module->memory.alloc(sizeof(FunType) * funCount);

    return type->args;
}

FunType* binaryFunType(FunType* type, Module* module, Type* lhs, Type* rhs, Type* result) {
    type->result = result;
    type->argCount = 2;
    type->args = (FunArg*)module->memory.alloc(sizeof(FunArg) * 2);

    type->args[0].name = 0;
    type->args[0].type = lhs;
    type->args[0].index = 0;

    type->args[1].name = 0;
    type->args[1].type = rhs;
    type->args[1].index = 1;

    return type;
}

Module* preludeModule(Context* context) {
    auto module = new Module;
    module->id = context->addUnqualifiedName("Prelude", 7);

    // Define basic operators.
    auto opEq = context->addUnqualifiedName("==", 2);
    auto opNeq = context->addUnqualifiedName("!=", 2);
    auto opLt = context->addUnqualifiedName("<", 1);
    auto opLe = context->addUnqualifiedName("<=", 2);
    auto opGt = context->addUnqualifiedName(">", 1);
    auto opGe = context->addUnqualifiedName(">=", 2);

    auto opPlus = context->addUnqualifiedName("+", 1);
    auto opMinus = context->addUnqualifiedName("-", 1);
    auto opMul = context->addUnqualifiedName("*", 1);
    auto opDiv = context->addUnqualifiedName("/", 1);
    auto opRem = context->addUnqualifiedName("rem", 3);

    auto opAnd = context->addUnqualifiedName("and", 3);
    auto opOr = context->addUnqualifiedName("or", 2);
    auto opXor = context->addUnqualifiedName("xor", 3);
    auto opShl = context->addUnqualifiedName("shl", 3);
    auto opShr = context->addUnqualifiedName("shr", 3);
    auto opSar = context->addUnqualifiedName("sar", 3);

    module->ops.add(opEq, OpProperties{4, Assoc::Left});
    module->ops.add(opNeq, OpProperties{4, Assoc::Left});
    module->ops.add(opLt, OpProperties{4, Assoc::Left});
    module->ops.add(opLe, OpProperties{4, Assoc::Left});
    module->ops.add(opGt, OpProperties{4, Assoc::Left});
    module->ops.add(opGe, OpProperties{4, Assoc::Left});

    module->ops.add(opPlus, OpProperties{6, Assoc::Left});
    module->ops.add(opMinus, OpProperties{6, Assoc::Left});
    module->ops.add(opMul, OpProperties{7, Assoc::Left});
    module->ops.add(opDiv, OpProperties{7, Assoc::Left});
    module->ops.add(opRem, OpProperties{7, Assoc::Left});

    module->ops.add(opAnd, OpProperties{7, Assoc::Left});
    module->ops.add(opOr, OpProperties{5, Assoc::Left});
    module->ops.add(opXor, OpProperties{6, Assoc::Left});
    module->ops.add(opShl, OpProperties{8, Assoc::Left});
    module->ops.add(opShr, OpProperties{8, Assoc::Left});

    // Define the basic types.
    module->types.add(context->addUnqualifiedName("Bool", 4), &intTypes[IntType::Bool]);
    module->types.add(context->addUnqualifiedName("Int", 3), &intTypes[IntType::Int]);
    module->types.add(context->addUnqualifiedName("Long", 4), &intTypes[IntType::Long]);
    module->types.add(context->addUnqualifiedName("Float", 5), &floatTypes[FloatType::F32]);
    module->types.add(context->addUnqualifiedName("Double", 6), &floatTypes[FloatType::F64]);
    module->types.add(context->addUnqualifiedName("String", 6), &stringType);

    auto orderingType = defineRecord(context, module, context->addUnqualifiedName("Ordering", 8), 3, false);
    orderingType->kind = RecordType::Enum;

    defineCon(context, module, orderingType, context->addUnqualifiedName("LT", 2), 0);
    defineCon(context, module, orderingType, context->addUnqualifiedName("EQ", 2), 1);
    defineCon(context, module, orderingType, context->addUnqualifiedName("GT", 2), 2);

    auto eqClass = defineClass(context, module, context->addUnqualifiedName("Eq", 2));
    {
        auto t = setClassType(module, eqClass, 2);
        defineClassFun(context, module, eqClass, opEq, 0);
        defineClassFun(context, module, eqClass, opNeq, 1);

        binaryFunType(&eqClass->functions[0], module, t, t, &intTypes[IntType::Bool]); // ==
        binaryFunType(&eqClass->functions[1], module, t, t, &intTypes[IntType::Bool]); // /=

        auto intInstance = [=](IntType* type) -> ClassInstance* {
            auto args = (Type**)module->memory.alloc(sizeof(Type*));
            args[0] = type;

            auto instance = defineInstance(context, module, eqClass, args);
            instance->instances[0] = cmpFunction<ICmp, icmp, ICmp::eq>(context, module, type, nullptr, 0);
            instance->instances[1] = cmpFunction<ICmp, icmp, ICmp::neq>(context, module, type, nullptr, 0);
            return instance;
        };

        auto floatInstance = [=](FloatType* type) -> ClassInstance* {
            auto args = (Type**)module->memory.alloc(sizeof(Type*));
            args[0] = type;

            auto instance = defineInstance(context, module, eqClass, args);
            instance->instances[0] = cmpFunction<FCmp, fcmp, FCmp::eq>(context, module, type, nullptr, 0);
            instance->instances[1] = cmpFunction<FCmp, fcmp, FCmp::neq>(context, module, type, nullptr, 0);
            return instance;
        };

        intInstance(&intTypes[IntType::Bool]);
        intInstance(&intTypes[IntType::Int]);
        intInstance(&intTypes[IntType::Long]);

        floatInstance(&floatTypes[FloatType::F16]);
        floatInstance(&floatTypes[FloatType::F32]);
        floatInstance(&floatTypes[FloatType::F64]);
    }

    auto ordClass = defineClass(context, module, context->addUnqualifiedName("Ord", 3));
    {
        auto t = setClassType(module, ordClass, 7);
        defineClassFun(context, module, ordClass, opLt, 0);
        defineClassFun(context, module, ordClass, opLe, 1);
        defineClassFun(context, module, ordClass, opGt, 2);
        defineClassFun(context, module, ordClass, opGe, 3);
        defineClassFun(context, module, ordClass, context->addUnqualifiedName("compare", 7), 4);
        defineClassFun(context, module, ordClass, context->addUnqualifiedName("min", 3), 5);
        defineClassFun(context, module, ordClass, context->addUnqualifiedName("max", 3), 6);

        binaryFunType(&ordClass->functions[0], module, t, t, &intTypes[IntType::Bool]); // <
        binaryFunType(&ordClass->functions[1], module, t, t, &intTypes[IntType::Bool]); // <=
        binaryFunType(&ordClass->functions[2], module, t, t, &intTypes[IntType::Bool]); // >
        binaryFunType(&ordClass->functions[3], module, t, t, &intTypes[IntType::Bool]); // >=
        binaryFunType(&ordClass->functions[4], module, t, t, orderingType); // compare
        binaryFunType(&ordClass->functions[5], module, t, t, t); // min
        binaryFunType(&ordClass->functions[6], module, t, t, t); // max

        auto intInstance = [=](IntType* type) -> ClassInstance* {
            auto args = (Type**)module->memory.alloc(sizeof(Type*));
            args[0] = type;

            auto instance = defineInstance(context, module, ordClass, args);
            instance->instances[0] = cmpFunction<ICmp, icmp, ICmp::ilt>(context, module, type, nullptr, 0);
            instance->instances[1] = cmpFunction<ICmp, icmp, ICmp::ile>(context, module, type, nullptr, 0);
            instance->instances[2] = cmpFunction<ICmp, icmp, ICmp::igt>(context, module, type, nullptr, 0);
            instance->instances[3] = cmpFunction<ICmp, icmp, ICmp::ige>(context, module, type, nullptr, 0);
            instance->instances[4] = ordCompare<ICmp, icmp, ICmp::eq, ICmp::igt>(context, module, type, orderingType, nullptr, 0);
            instance->instances[5] = maxCompare<ICmp, icmp, ICmp::igt>(context, module, type, nullptr, 0);
            instance->instances[6] = maxCompare<ICmp, icmp, ICmp::ilt>(context, module, type, nullptr, 0);
            return instance;
        };

        auto floatInstance = [=](FloatType* type) -> ClassInstance* {
            auto args = (Type**)module->memory.alloc(sizeof(Type*));
            args[0] = type;

            auto instance = defineInstance(context, module, ordClass, args);
            instance->instances[0] = cmpFunction<FCmp, fcmp, FCmp::lt>(context, module, type, nullptr, 0);
            instance->instances[1] = cmpFunction<FCmp, fcmp, FCmp::le>(context, module, type, nullptr, 0);
            instance->instances[2] = cmpFunction<FCmp, fcmp, FCmp::gt>(context, module, type, nullptr, 0);
            instance->instances[3] = cmpFunction<FCmp, fcmp, FCmp::ge>(context, module, type, nullptr, 0);
            instance->instances[4] = ordCompare<FCmp, fcmp, FCmp::eq, FCmp::gt>(context, module, type, orderingType, nullptr, 0);
            instance->instances[5] = maxCompare<FCmp, fcmp, FCmp::gt>(context, module, type, nullptr, 0);
            instance->instances[6] = maxCompare<FCmp, fcmp, FCmp::lt>(context, module, type, nullptr, 0);
            return instance;
        };

        intInstance(&intTypes[IntType::Bool]);
        intInstance(&intTypes[IntType::Int]);
        intInstance(&intTypes[IntType::Long]);

        floatInstance(&floatTypes[FloatType::F16]);
        floatInstance(&floatTypes[FloatType::F32]);
        floatInstance(&floatTypes[FloatType::F64]);
    }

    auto numClass = defineClass(context, module, context->addUnqualifiedName("Num", 3));
    {
        auto t = setClassType(module, numClass, 4);
        defineClassFun(context, module, numClass, opPlus, 0);
        defineClassFun(context, module, numClass, opMinus, 1);
        defineClassFun(context, module, numClass, opMul, 2);
        defineClassFun(context, module, numClass, opDiv, 3);

        binaryFunType(&numClass->functions[0], module, t, t, t); // +
        binaryFunType(&numClass->functions[1], module, t, t, t); // -
        binaryFunType(&numClass->functions[2], module, t, t, t); // *
        binaryFunType(&numClass->functions[3], module, t, t, t); // /

        auto intInstance = [=](IntType* type) -> ClassInstance* {
            auto args = (Type**)module->memory.alloc(sizeof(Type*));
            args[0] = type;

            auto instance = defineInstance(context, module, numClass, args);
            instance->instances[0] = binaryFunction<add>(context, module, type, nullptr, 0);
            instance->instances[1] = binaryFunction<sub>(context, module, type, nullptr, 0);
            instance->instances[2] = binaryFunction<mul>(context, module, type, nullptr, 0);
            instance->instances[3] = binaryFunction<div>(context, module, type, nullptr, 0);
            return instance;
        };

        auto floatInstance = [=](FloatType* type) -> ClassInstance* {
            auto args = (Type**)module->memory.alloc(sizeof(Type*));
            args[0] = type;

            auto instance = defineInstance(context, module, numClass, args);
            instance->instances[0] = binaryFunction<fadd>(context, module, type, nullptr, 0);
            instance->instances[1] = binaryFunction<fsub>(context, module, type, nullptr, 0);
            instance->instances[2] = binaryFunction<fmul>(context, module, type, nullptr, 0);
            instance->instances[3] = binaryFunction<fdiv>(context, module, type, nullptr, 0);
            return instance;
        };

        intInstance(&intTypes[IntType::Bool]);
        intInstance(&intTypes[IntType::Int]);
        intInstance(&intTypes[IntType::Long]);

        floatInstance(&floatTypes[FloatType::F16]);
        floatInstance(&floatTypes[FloatType::F32]);
        floatInstance(&floatTypes[FloatType::F64]);
    }

    auto integralClass = defineClass(context, module, context->addQualifiedName("Integral", 8));
    {
        auto t = setClassType(module, integralClass, 7);
        defineClassFun(context, module, integralClass, opShl, 0);
        defineClassFun(context, module, integralClass, opSar, 1);
        defineClassFun(context, module, integralClass, opShr, 2);
        defineClassFun(context, module, integralClass, opAnd, 3);
        defineClassFun(context, module, integralClass, opOr, 4);
        defineClassFun(context, module, integralClass, opXor, 5);
        defineClassFun(context, module, integralClass, opRem, 6);

        binaryFunType(&integralClass->functions[0], module, t, &intTypes[IntType::Int], t); // shl
        binaryFunType(&integralClass->functions[1], module, t, &intTypes[IntType::Int], t); // sar
        binaryFunType(&integralClass->functions[2], module, t, &intTypes[IntType::Int], t); // shr
        binaryFunType(&integralClass->functions[3], module, t, t, t); // and
        binaryFunType(&integralClass->functions[4], module, t, t, t); // or
        binaryFunType(&integralClass->functions[5], module, t, t, t); // xor
        binaryFunType(&integralClass->functions[6], module, t, t, t); // rem

        auto intInstance = [=](IntType* type) -> ClassInstance* {
            auto args = (Type**)module->memory.alloc(sizeof(Type*));
            args[0] = type;

            auto instance = defineInstance(context, module, integralClass, args);
            instance->instances[0] = binaryFunction<shl>(context, module, type, nullptr, 0);
            instance->instances[1] = binaryFunction<sar>(context, module, type, nullptr, 0);
            instance->instances[2] = binaryFunction<shr>(context, module, type, nullptr, 0);
            instance->instances[3] = binaryFunction<and_>(context, module, type, nullptr, 0);
            instance->instances[4] = binaryFunction<or_>(context, module, type, nullptr, 0);
            instance->instances[5] = binaryFunction<xor_>(context, module, type, nullptr, 0);
            instance->instances[6] = binaryFunction<rem>(context, module, type, nullptr, 0);
            return instance;
        };

        intInstance(&intTypes[IntType::Bool]);
        intInstance(&intTypes[IntType::Int]);
        intInstance(&intTypes[IntType::Long]);
    }

    auto printFunction = defineFun(context, module, context->addUnqualifiedName("print", 5));
    {
        printFunction->returnType = &unitType;

        auto arg = defineArg(context, printFunction, 0, &stringType);
        auto body = block(printFunction);

        auto args = (Value**)module->memory.alloc(sizeof(Value*) * 3);
        args[0] = constInt(body, 0, 1, &intTypes[IntType::Int]);
        args[1] = stringData(body, 0, arg);
        args[2] = stringLength(body, 0, arg);

        callDyn(body, 0, constInt(body, 0, 1, &intTypes[IntType::Int]), &unitType, args, 3, true);
        ret(body);
    }

    auto exitFunction = defineFun(context, module, context->addUnqualifiedName("exitProgram", 11));
    {
        exitFunction->returnType = &unitType;

        auto code = defineArg(context, exitFunction, 0, &intTypes[IntType::Int]);
        auto body = block(exitFunction);

        auto args = (Value**)module->memory.alloc(sizeof(Value*));
        args[0] = code;

        callDyn(body, 0, constInt(body, 0, 60, &intTypes[IntType::Int]), &unitType, args, 1, true);
        ret(body);
    }

    return module;
}

Module* unsafeModule(Context* context, Module* prelude) {
    auto module = new Module;
    module->id = context->addUnqualifiedName("Unsafe", 6);

    return module;
}