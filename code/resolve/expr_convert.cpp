#include "expr.h"
#include "../util/string.h"

/*
 * Handles conversions between types.
 * Most conversions are implicit, since explicit ones are mostly implemented as builtin functions.
 * Implicit conversions can never remove data, unless part of an explicit type construction.
 */

static Value* implicitConvertInt(FunBuilder* b, Value* v, Type* targetType, bool isConstruct) {
    // Integer types can be implicitly extended to a larger type.
    auto intType = (IntType*)v->type;
    auto targetInt = (IntType*)targetType;
    if(intType->bits == targetInt->bits) {
        return v;
    } else if(intType->bits < targetInt->bits) {
        return sext(b->block, 0, v, targetType);
    } else if(isConstruct) {
        return trunc(b->block, 0, v, targetType);
    } else {
        error(b, "cannot implicitly convert an integer to a smaller type", nullptr);
        return v;
    }
}

static Value* implicitConvertFloat(FunBuilder* b, Value* v, Type* targetType, bool isConstruct) {
    // Float types can be implicitly extended to a larger type.
    auto floatType = (FloatType*)v->type;
    auto targetFloat = (FloatType*)targetType;
    if(floatType->bits == targetFloat->bits) {
        return v;
    } else if(floatType->bits < targetFloat->bits) {
        return fext(b->block, 0, v, targetType);
    } else if(isConstruct) {
        return ftrunc(b->block, 0, v, targetType);
    } else {
        error(b, "cannot implicitly convert a float to a smaller type", nullptr);
        return v;
    }
}

// Adds code to implicitly convert a value if possible.
// If isConstruct is set, the conversion is less strict since the target type is explicitly defined.
// Returns the new value or null if no conversion is possible.
Value* implicitConvert(FunBuilder* b, Value* v, Type* targetType, bool isConstruct, bool require) {
    auto type = v->type;
    if(compareTypes(&b->context, type, targetType)) return v;

    auto kind = type->kind;
    auto targetKind = targetType->kind;

    // TODO: Support conversions between equivalent tuple types.
    // TODO: Support conversions to generic types.
    if(kind == Type::Ref && targetKind == Type::Ref) {
        // Different reference kinds can be implicitly converted in some cases.
        auto ref = (RefType*)type;
        auto targetRef = (RefType*)targetType;

        // This case is handled implicitly by the code generators.
        if(ref->isMutable && !targetRef->isMutable) {
            return v;
        }

        // A local reference can be implicitly converted to a traced one.
        // Following the semantics of the language, the contained value is copied.
        if(ref->isLocal && targetRef->isTraced) {
            auto newRef = alloc(b->block, 0, targetRef->to, targetRef->isMutable, false);
            load(b->block, 0, v);
            store(b->block, 0, newRef, v);
            return newRef;
        }

        // All other cases are unsafe (traced to untraced)
        // or would have unexpected side-effects (copying untraced to traced).
    } else if(kind == Type::Ref) {
        // A returned reference can be implicitly loaded to a register.
        auto ref = (RefType*)type;
        if(compareTypes(&b->context, ref->to, targetType)) {
            return load(b->block, 0, v);
        } else if(
            (ref->to->kind == Type::Int && targetType->kind == Type::Int) &&
            (((IntType*)ref->to)->bits <= ((IntType*)targetType)->bits || isConstruct)
        ) {
            v = load(b->block, 0, v);
            return implicitConvertInt(b, v, targetType, isConstruct);
        } else if(
            (ref->to->kind == Type::Float && targetType->kind == Type::Float) &&
            (((FloatType*)ref->to)->bits <= ((FloatType*)targetType)->bits || isConstruct)
        ) {
            v = load(b->block, 0, v);
            return implicitConvertFloat(b, v, targetType, isConstruct);
        }
    } else if(kind == Type::Int && targetKind == Type::Int) {
        return implicitConvertInt(b, v, targetType, isConstruct);
    } else if(kind == Type::Float && targetKind == Type::Float) {
        return implicitConvertFloat(b, v, targetType, isConstruct);
    } else if(isConstruct && kind == Type::Float && targetKind == Type::Int) {
        return ftoi(b->block, 0, v, targetType);
    } else if(isConstruct && kind == Type::Int && targetKind == Type::Float) {
        return itof(b->block, 0, v, targetType);
    } else if(isConstruct && kind == Type::String && targetKind == Type::Int && v->kind == Value::ConstString) {
        // A string literal with a single code point can be converted to an integer.
        auto string = (ConstString*)v;
        U32 codePoint = 0;
        if(string->length > 0) {
            auto bytes = string->value;
            convertNextPoint(bytes, &codePoint);
            if(string->length > (bytes - string->value)) {
                error(b, "cannot convert a string with multiple characters to an int", nullptr);
            }
        }

        return constInt(b->block, 0, codePoint, &intTypes[IntType::Int]);
    }

    if(require) {
        error(b, "cannot implicitly convert to type", nullptr);
        return error(b->block, 0, targetType);
    } else {
        return nullptr;
    }
}