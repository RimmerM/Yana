#include "type.h"

Type unitType{Type::Unit};
Type stringType{Type::String};

FloatType floatTypes[FloatType::KindCount] = {
    {FloatType::F16},
    {FloatType::F32},
    {FloatType::F64}
};

IntType intTypes[IntType::KindCount] = {
    {1, IntType::Bool},
    {32, IntType::Int},
    {64, IntType::Long}
};
