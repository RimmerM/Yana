#include "module.h"

bool isTerminator(const Value& value) {
    return value.kind == Value::Je || value.kind == Value::Jmp || value.kind == Value::Ret;
}

bool isConstant(const Value& value) {
    return value.kind == Value::ConstInt || value.kind == Value::ConstFloat || value.kind == Value::ConstDouble;
}

Size instructionPlaces(const Value& instruction, Place* target) {
    switch(instruction.kind) {
        case Value::LoadPlace: target[0] = ((const InstLoadPlace&)instruction).place; return 1;
        case Value::Init:
        case Value::Assign: target[0] = ((const InstInit&)instruction).place; return 1;
        case Value::Borrow: target[0] = ((const InstBorrow&)instruction).place; return 1;
        case Value::Move: target[0] = ((const InstMove&)instruction).place; return 1;
        case Value::Copy: target[0] = ((const InstCopy&)instruction).place; return 1;
        case Value::Drop: target[0] = ((const InstDrop&)instruction).place; return 1;
        case Value::Address: target[0] = ((const InstAddress&)instruction).place; return 1;
        case Value::Exchange: target[0] = ((const InstExchange&)instruction).place; return 1;
        case Value::Swap:
            target[0] = ((const InstSwap&)instruction).a;
            target[1] = ((const InstSwap&)instruction).b;
            return 2;
        default: return 0;
    }
}

Size instructionPlaceSlots(Value& instruction, Place** target) {
    switch(instruction.kind) {
        case Value::LoadPlace: target[0] = &((InstLoadPlace&)instruction).place; return 1;
        case Value::Init:
        case Value::Assign: target[0] = &((InstInit&)instruction).place; return 1;
        case Value::Borrow: target[0] = &((InstBorrow&)instruction).place; return 1;
        case Value::Move: target[0] = &((InstMove&)instruction).place; return 1;
        case Value::Copy: target[0] = &((InstCopy&)instruction).place; return 1;
        case Value::Drop: target[0] = &((InstDrop&)instruction).place; return 1;
        case Value::Address: target[0] = &((InstAddress&)instruction).place; return 1;
        case Value::Exchange: target[0] = &((InstExchange&)instruction).place; return 1;
        case Value::Swap:
            target[0] = &((InstSwap&)instruction).a;
            target[1] = &((InstSwap&)instruction).b;
            return 2;
        default: return 0;
    }
}

StringView conventionName(ast::BindType convention) {
    switch(convention) {
        case ast::BindType::Ref: return "`&`"_v;
        case ast::BindType::Sink: return "`->`"_v;
        default: return "an immutable borrow"_v;
    }
}

StringView funValueFieldName(U16 field) {
    switch(field) {
        case FunValueLayout::kCode: return "code"_v;
        case FunValueLayout::kHeader: return "header"_v;
        default: return "env"_v;
    }
}
