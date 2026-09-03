#include "decl.h"

namespace mir {

StringView binaryOpName(BinaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width) case BinaryOp::kind: return mnemonic;
#include "op.def"
        case BinaryOp::Count: break;
    }

    return "?"_v;
}

StringView unaryOpName(UnaryOp op) {
    switch(op) {
#define YANA_MIR_UNOP(kind, mnemonic, category, width) case UnaryOp::kind: return mnemonic;
#include "op.def"
        case UnaryOp::Count: break;
    }

    return "?"_v;
}

OpCategory binaryCategory(BinaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width) case BinaryOp::kind: return category;
#include "op.def"
        case BinaryOp::Count: break;
    }

    assertTrue(false);
    return OpCategory::Arith;
}

UnaryCategory unaryCategory(UnaryOp op) {
    switch(op) {
#define YANA_MIR_UNOP(kind, mnemonic, category, width) case UnaryOp::kind: return category;
#include "op.def"
        case UnaryOp::Count: break;
    }

    assertTrue(false);
    return UnaryCategory::Arith;
}

WidthEffect binaryWidthEffect(BinaryOp op) {
    switch(op) {
#define YANA_MIR_BINOP(kind, mnemonic, category, width) case BinaryOp::kind: return width;
#include "op.def"
        case BinaryOp::Count: break;
    }

    assertTrue(false);
    return WidthEffect::Closed;
}

WidthEffect unaryWidthEffect(UnaryOp op) {
    switch(op) {
#define YANA_MIR_UNOP(kind, mnemonic, category, width) case UnaryOp::kind: return width;
#include "op.def"
        case UnaryOp::Count: break;
    }

    assertTrue(false);
    return WidthEffect::Closed;
}

StringView atomicOpName(AtomicOp op) {
    switch(op) {
#define YANA_MIR_ATOMIC(kind, mnemonic, form) case AtomicOp::kind: return mnemonic;
#include "op.def"
        case AtomicOp::Count: break;
    }

    return "?"_v;
}

AtomicForm atomicForm(AtomicOp op) {
    switch(op) {
#define YANA_MIR_ATOMIC(kind, mnemonic, form) case AtomicOp::kind: return form;
#include "op.def"
        case AtomicOp::Count: break;
    }

    assertTrue(false);
    return AtomicForm::Bare;
}

// The words `doc/spec/concurrency.md` uses, so a dump reads as the model does.
StringView atomicOrderName(AtomicOrder order) {
    switch(order) {
        case AtomicOrder::Relaxed:        return "relaxed"_v;
        case AtomicOrder::Acquire:        return "acquire"_v;
        case AtomicOrder::Release:        return "release"_v;
        case AtomicOrder::AcquireRelease: return "acqrel"_v;
        case AtomicOrder::Sequential:     return "seqcst"_v;
    }

    return "?"_v;
}

StringView reduceOpName(ReduceOp op) {
    switch(op) {
#define YANA_MIR_REDUCE(kind, mnemonic) case ReduceOp::kind: return mnemonic;
#include "op.def"
        case ReduceOp::Count: break;
    }

    return "?"_v;
}

// An enum-layout record, whose value is its tag - what a comparison compares.
static bool isEnumRecord(DeclBase base, DeclPtr<Type> type) {
    return base[type]->kind == Type::Record && ((RecordType*)base[type])->layout == RecordLayout::Enum;
}

/*
 * What a vector admits, which is the lane's own answer with two subtractions.
 *
 * A vector of a lane type is exactly as arithmetic as the lane is - an add of two `Vec(I32)` is the
 * add of two `I32` per lane - so the rule reads the lane rather than restating the categories.
 * `Vec(a)` inside a generic body reaches this with a variable lane and is admitted: a lane that was
 * not a machine primitive would have been refused where the type was made, so there is nothing here
 * a substitution could turn into a mistake.
 *
 * The two subtractions are both a **mask**'s. Its lanes are all-ones or all-zeros, so every
 * arithmetic operation would produce something that is neither - which is why `Bitwise` is the class
 * a mask joins and `Num` is not. And nothing compares two masks: what a comparison of them would
 * answer is a mask of a mask, and the operations that read one back are the reductions.
 */
static bool vectorOperandAllowed(DeclBase base, OpCategory category, const VectorType& vector) {
    auto lane = base[vector.lane]->kind;
    auto numeric = lane == Type::Int || lane == Type::Float || lane == Type::Variable;
    auto integral = lane == Type::Int || lane == Type::Variable;

    if(vector.isMask) return category == OpCategory::Bits;

    switch(category) {
        case OpCategory::Arith:
            return numeric;
        case OpCategory::Bits:
        case OpCategory::Integer:
        case OpCategory::Shift:
            return integral;

        // A lanewise comparison, whose answer is the mask `binaryResult` states - Design-Vector
        // §3.2, and the reason `Eq` and `Ord` have no vector instances.
        case OpCategory::Equality:
        case OpCategory::Order:
            return numeric;
    }

    return false;
}

bool operandTypeAllowed(Program& program, OpCategory category, DeclPtr<Type> type) {
    auto base = *program.decl;
    auto kind = base[type]->kind;

    if(kind == Type::Vector) {
        return vectorOperandAllowed(base, category, *(VectorType*)base[type]);
    }

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
    auto base = *program.decl;
    auto kind = base[type]->kind;

    /*
     * A vector's unary operations are its lane's, with the same two answers `vectorOperandAllowed`
     * gives: `neg` is arithmetic and `not` is bits, and a **mask** has the second alone. A mask's
     * `not` is the complement of every bit and not `Bool`'s `xor 1` - a mask lane is all-ones or
     * all-zeros, so complementing it lands back inside the type where a single flipped bit would not.
     *
     * A reinterpretation of a vector is admitted, which is `Bitcast`'s rungs over equal byte widths
     * and the one way a mask crosses to another lane type - see `provideVectorInstances`.
     */
    if(kind == Type::Vector) {
        auto& vector = *(VectorType*)base[type];
        auto lane = base[vector.lane]->kind;

        if(category == UnaryCategory::Raw) return true;
        if(vector.isMask) return category == UnaryCategory::Bits;

        if(category == UnaryCategory::Bits) return lane == Type::Int || lane == Type::Variable;
        if(category == UnaryCategory::Float) return lane == Type::Float || lane == Type::Variable;
        return lane == Type::Int || lane == Type::Float || lane == Type::Variable;
    }

    switch(category) {
        case UnaryCategory::Arith:
            return kind == Type::Int || kind == Type::Float;
        case UnaryCategory::Bits:
            return kind == Type::Int || type == booleanType(program);

        // An address is one of these, which is the whole of what the row adds - see `UnaryCategory`.
        case UnaryCategory::Raw:
            return kind == Type::Int || kind == Type::Float || kind == Type::Pointer;

        // A float and nothing else - see `UnaryCategory::Float`, where the reason it is a category
        // rather than a test at each emitter is written.
        case UnaryCategory::Float:
            return kind == Type::Float;
    }

    return false;
}

} // namespace mir
