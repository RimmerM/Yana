#include "decl.h"

namespace mir {

StringView binaryOpName(BinaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width) case BinaryOp::kind: return mnemonic;
#define YANA_MIR_UNOP(kind, mnemonic, category, width)
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
        case BinaryOp::Count: break;
    }

    return "?"_v;
}

StringView unaryOpName(UnaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width)
#define YANA_MIR_UNOP(kind, mnemonic, category, width) case UnaryOp::kind: return mnemonic;
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
        case UnaryOp::Count: break;
    }

    return "?"_v;
}

OpCategory binaryCategory(BinaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width) case BinaryOp::kind: return category;
#define YANA_MIR_UNOP(kind, mnemonic, category, width)
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
        case BinaryOp::Count: break;
    }

    assertTrue(false);
    return OpCategory::Arith;
}

UnaryCategory unaryCategory(UnaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width)
#define YANA_MIR_UNOP(kind, mnemonic, category, width) case UnaryOp::kind: return category;
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
        case UnaryOp::Count: break;
    }

    assertTrue(false);
    return UnaryCategory::Arith;
}

WidthEffect binaryWidthEffect(BinaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width) case BinaryOp::kind: return width;
#define YANA_MIR_UNOP(kind, mnemonic, category, width)
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
        case BinaryOp::Count: break;
    }

    assertTrue(false);
    return WidthEffect::Closed;
}

WidthEffect unaryWidthEffect(UnaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width)
#define YANA_MIR_UNOP(kind, mnemonic, category, width) case UnaryOp::kind: return width;
#include "op.def"
#undef YANA_MIR_UNOP
#undef YANA_MIR_BINOP
        case UnaryOp::Count: break;
    }

    assertTrue(false);
    return WidthEffect::Closed;
}

// An enum-layout record, whose value is its tag - what a comparison compares.
static bool isEnumRecord(DeclBase base, DeclPtr<Type> type) {
    return base[type]->kind == Type::Record && ((RecordType*)base[type])->layout == RecordLayout::Enum;
}

bool operandTypeAllowed(Program& program, OpCategory category, DeclPtr<Type> type) {
    auto base = *program.decl;
    auto kind = base[type]->kind;

    switch(category) {
        case OpCategory::Arith:
            return kind == Type::Int || kind == Type::Float;

        // A pointer **orders**, which is the one thing about addresses that is not a bit comparison:
        // `heapNext + block > heapLimit` is how storage carved out of a region knows it has run out,
        // and there is nothing else in the language to ask. Arithmetic on one is *not* here - `p + n`
        // scales by the pointee's stride and is `pointerAdd`, which no `Num` instance could express.
        case OpCategory::Order:
            return kind == Type::Int || kind == Type::Float || kind == Type::Pointer;

        case OpCategory::Bits:
            return kind == Type::Int || type == booleanType(program);
        case OpCategory::Integer:
        case OpCategory::Shift:
            return kind == Type::Int;

        // A pointer compares because comparing two addresses is what the unsafe boundary is for,
        // `{}` because a type with one value has one answer, and an enum because its value is its
        // tag - none of them is an ordering.
        case OpCategory::Equality:
            return kind == Type::Int || kind == Type::Float || kind == Type::Pointer ||
                   kind == Type::Unit || isEnumRecord(base, type);
    }

    return false;
}

bool operandTypeAllowed(Program& program, UnaryCategory category, DeclPtr<Type> type) {
    auto kind = (*program.decl)[type]->kind;

    switch(category) {
        case UnaryCategory::Arith:
            return kind == Type::Int || kind == Type::Float;
        case UnaryCategory::Bits:
            return kind == Type::Int || type == booleanType(program);

        // An address is one of these, which is the whole of what the row adds - see `UnaryCategory`.
        case UnaryCategory::Raw:
            return kind == Type::Int || kind == Type::Float || kind == Type::Pointer;
    }

    return false;
}

} // namespace mir
