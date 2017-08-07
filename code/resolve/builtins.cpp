
#include "module.h"

Module* preludeModule(Context* context) {
    auto module = new Module;
    module->name = &context->find(context->addUnqualifiedName("Prelude", 7));

    // Define basic operators.
    auto opEq = context->addUnqualifiedName("==", 2);
    auto opNeq = context->addUnqualifiedName("/=", 2);
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

    auto ltCon = defineCon(context, module, orderingType, context->addUnqualifiedName("LT", 2), 0, nullptr);
    auto eqCon = defineCon(context, module, orderingType, context->addUnqualifiedName("EQ", 2), 1, nullptr);
    auto gtCon = defineCon(context, module, orderingType, context->addUnqualifiedName("GT", 2), 2, nullptr);

    auto eqClass = defineClass(context, module, context->addUnqualifiedName("Eq", 2));
    {
        auto t = new (module->memory) GenType(0);
    }

    return module;
}

Module* unsafeModule(Context* context) {
    auto module = new Module;
    module->name = &context->find(context->addUnqualifiedName("Unsafe", 6));



    return module;
}