#include "inst.h"

bool isTerminator(const Value& value) {
    return value.kind == Value::Je || value.kind == Value::Jmp || value.kind == Value::Ret;
}

bool isConstant(const Value& value) {
    return value.kind == Value::ConstInt || value.kind == Value::ConstFloat || value.kind == Value::ConstDouble;
}
