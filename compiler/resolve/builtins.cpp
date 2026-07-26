#include "builtins.h"
#include "builder.h"

/*
 * The scalar operators, as ordinary resolve-IR functions.
 *
 * Each one is a real Function with real arguments and a real body, generated here instead of
 * parsed - so a call to `+` is an ordinary call, selected by the same overload resolution as any
 * other, and lowered by the same code. Inlining them is the backend's business, not the
 * resolver's. They are the temporary stand-in for the Num/Ord/Integral instances of
 * Implementation-IR.md part 6, and only the selection rule in expr_call.cpp has to go when those
 * land - not this file's shape.
 */

static StringId name(Module& module, StringView value) {
    return module.context.addQualifiedName(value.ptr, value.length, 1);
}

static Function* beginBuiltin(Module& module, StringView internalName, StringView exportedName, TypePtr result) {
    auto function = module.addFunction(name(module, internalName), kNullLocation);
    function->builtin = true;
    function->exportedName = name(module, exportedName);
    function->returnType = result;

    module.overloads.push(module.arena, FunctionOverload {
        function->exportedName,
        function - *module.arena,
        true,
    });

    return function;
}

// A second name for a function that already exists, for the operators that have one: `rem` is
// also `%`, `not` is also `~`.
static void aliasBuiltin(Module& module, Function& function, StringView exportedName) {
    module.overloads.push(module.arena, FunctionOverload {
        name(module, exportedName),
        &function - *module.arena,
        true,
    });
}

static void finishBuiltin(Module& module, Function& function, Value* result) {
    auto base = *module.arena;
    auto block = module.entry(function);

    addInst<InstRet>(module, function, *block, kNullLocation, 0, module.scalar.unit, result ? result - base : nullptr);
}

static Function* binaryBuiltin(Module& module, StringView internalName, StringView exportedName, TypePtr type, Value::Kind kind) {
    auto function = beginBuiltin(module, internalName, exportedName, type);
    auto lhs = function->addArg(module, name(module, "lhs"_v), type, kNullLocation);
    auto rhs = function->addArg(module, name(module, "rhs"_v), type, kNullLocation);
    auto base = *module.arena;
    auto block = module.entry(*function);

    auto result = addInst<InstBinary>(module, *function, *block, kNullLocation, 0, type, kind, (Value*)lhs - base, (Value*)rhs - base);
    finishBuiltin(module, *function, result);

    return function;
}

static Function* compareBuiltin(Module& module, StringView internalName, StringView exportedName, TypePtr type, CompareOp compare) {
    auto function = beginBuiltin(module, internalName, exportedName, module.scalar.bool_);
    auto lhs = function->addArg(module, name(module, "lhs"_v), type, kNullLocation);
    auto rhs = function->addArg(module, name(module, "rhs"_v), type, kNullLocation);
    auto base = *module.arena;
    auto block = module.entry(*function);

    auto result = addInst<InstCmp>(module, *function, *block, kNullLocation, 0, module.scalar.bool_, (Value*)lhs - base, (Value*)rhs - base, compare);
    finishBuiltin(module, *function, result);

    return function;
}

static Function* unaryBuiltin(Module& module, StringView internalName, StringView exportedName, TypePtr type, Value::Kind kind) {
    auto function = beginBuiltin(module, internalName, exportedName, type);
    auto arg = function->addArg(module, name(module, "value"_v), type, kNullLocation);
    auto base = *module.arena;
    auto block = module.entry(*function);

    auto result = addInst<InstUnary>(module, *function, *block, kNullLocation, 0, type, kind, (Value*)arg - base);
    finishBuiltin(module, *function, result);

    return function;
}

// Bool is a two-constructor record rather than an integer type, so its negation is written as
// the one bit operation that is correct for a discriminant rather than as an integer `not`.
static Function* boolNotBuiltin(Module& module) {
    auto function = beginBuiltin(module, "__builtin_bool_not"_v, "not"_v, module.scalar.bool_);
    auto arg = function->addArg(module, name(module, "value"_v), module.scalar.bool_, kNullLocation);
    auto base = *module.arena;
    auto block = module.entry(*function);

    auto one = addConstant<ConstInt>(module, *function, *block, kNullLocation, module.scalar.bool_, U64(1));
    auto result = addInst<InstBinary>(module, *function, *block, kNullLocation, 0, module.scalar.bool_, Value::Xor, (Value*)arg - base, (Value*)one - base);
    finishBuiltin(module, *function, result);

    return function;
}

static void defineNumeric(Module& module, TypePtr type, StringView tag) {
    static const struct {
        StringView suffix;
        StringView exported;
        Value::Kind kind;
    } arithmetic[] = {
        { "add"_v, "+"_v, Value::Add },
        { "sub"_v, "-"_v, Value::Sub },
        { "mul"_v, "*"_v, Value::Mul },
        { "div"_v, "/"_v, Value::Div },
    };

    static const struct {
        StringView suffix;
        StringView exported;
        CompareOp compare;
    } comparisons[] = {
        { "eq"_v, "=="_v, CompareOp::Eq },
        { "ne"_v, "!="_v, CompareOp::Ne },
        { "lt"_v, "<"_v, CompareOp::Lt },
        { "le"_v, "<="_v, CompareOp::Le },
        { "gt"_v, ">"_v, CompareOp::Gt },
        { "ge"_v, ">="_v, CompareOp::Ge },
    };

    char storage[64];
    for(auto& operation: arithmetic) {
        auto length = format(Buffer<char> { storage, 64 }, "__builtin_%@_%@", tag, operation.suffix);
        binaryBuiltin(module, { storage, length }, operation.exported, type, operation.kind);
    }

    for(auto& operation: comparisons) {
        auto length = format(Buffer<char> { storage, 64 }, "__builtin_%@_%@", tag, operation.suffix);
        compareBuiltin(module, { storage, length }, operation.exported, type, operation.compare);
    }

    auto length = format(Buffer<char> { storage, 64 }, "__builtin_%@_neg", tag);
    unaryBuiltin(module, { storage, length }, "-"_v, type, Value::Neg);
}

// The operations only the integer types have: everything that is about a value's bits rather
// than about its magnitude, plus the remainder that floating division does not provide.
static void defineInteger(Module& module, TypePtr type, StringView tag) {
    static const struct {
        StringView suffix;
        StringView exported;
        Value::Kind kind;
    } operations[] = {
        { "rem"_v, "rem"_v, Value::Rem },
        { "shl"_v, "shl"_v, Value::Shl },
        { "shr"_v, "shr"_v, Value::Shr },
        { "sar"_v, "sar"_v, Value::Sar },
        { "and"_v, "and"_v, Value::And },
        { "or"_v, "or"_v, Value::Or },
        { "xor"_v, "xor"_v, Value::Xor },
    };

    char storage[64];
    for(auto& operation: operations) {
        auto length = format(Buffer<char> { storage, 64 }, "__builtin_%@_%@", tag, operation.suffix);
        auto function = binaryBuiltin(module, { storage, length }, operation.exported, type, operation.kind);
        if(operation.kind == Value::Rem) aliasBuiltin(module, *function, "%"_v);
    }

    auto length = format(Buffer<char> { storage, 64 }, "__builtin_%@_not", tag);
    auto function = unaryBuiltin(module, { storage, length }, "not"_v, type, Value::Not);
    aliasBuiltin(module, *function, "~"_v);
}

void defineBuiltins(Module& module) {
    defineNumeric(module, module.scalar.int_, "i32"_v);
    defineNumeric(module, module.scalar.long_, "i64"_v);
    defineNumeric(module, module.scalar.float_, "f32"_v);
    defineNumeric(module, module.scalar.double_, "f64"_v);
    defineInteger(module, module.scalar.int_, "i32"_v);
    defineInteger(module, module.scalar.long_, "i64"_v);

    compareBuiltin(module, "__builtin_bool_eq"_v, "=="_v, module.scalar.bool_, CompareOp::Eq);
    compareBuiltin(module, "__builtin_bool_ne"_v, "!="_v, module.scalar.bool_, CompareOp::Ne);

    auto boolAnd = binaryBuiltin(module, "__builtin_bool_and"_v, "and"_v, module.scalar.bool_, Value::And);
    aliasBuiltin(module, *boolAnd, "&&"_v);

    auto boolOr = binaryBuiltin(module, "__builtin_bool_or"_v, "or"_v, module.scalar.bool_, Value::Or);
    aliasBuiltin(module, *boolOr, "||"_v);

    binaryBuiltin(module, "__builtin_bool_xor"_v, "xor"_v, module.scalar.bool_, Value::Xor);

    auto boolNot = boolNotBuiltin(module);
    aliasBuiltin(module, *boolNot, "~"_v);
    aliasBuiltin(module, *boolNot, "!"_v);

    auto precedence = [&](StringView op, U8 value) {
        module.operatorPrecedence.add(name(module, op), value);
    };

    precedence("||"_v, 1);
    precedence("or"_v, 1);
    precedence("&&"_v, 2);
    precedence("and"_v, 2);
    precedence("=="_v, 3);
    precedence("!="_v, 3);
    precedence(">"_v, 3);
    precedence(">="_v, 3);
    precedence("<"_v, 3);
    precedence("<="_v, 3);
    precedence("xor"_v, 4);
    precedence("shl"_v, 5);
    precedence("shr"_v, 5);
    precedence("sar"_v, 5);
    precedence("+"_v, 6);
    precedence("-"_v, 6);
    precedence("*"_v, 7);
    precedence("/"_v, 7);
    precedence("rem"_v, 7);
    precedence("%"_v, 7);
}
