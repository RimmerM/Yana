#pragma once

#include "module.h"

namespace ast {
    struct ConExpr;
    struct Pat;
    struct Literal;
    struct TupArg;
}

/*
 * Helper functions for instruction generation.
 */

template<class... T>
inline void error(FunBuilder* b, const char* message, const Node* node, T&&... p) {
    b->context.diagnostics.error(message, node, nullptr, forward<T>(p)...);
}

Value* implicitConvert(FunBuilder* b, Value* v, Type* targetType, bool isConstruct, bool require);

Value* findVar(FunBuilder* b, Id name);
Value* useValue(FunBuilder* b, Value* value, bool asRV);

Value* resolveStaticCall(FunBuilder* b, Id funName, Value* firstArg, List<ast::TupArg>* argList, Id name);
Value* genStaticCall(FunBuilder* b, Id funName, Value** args, U32 count, Id name);
Value* resolveDynCall(FunBuilder* b, Value* callee, List<ast::TupArg>* argList, Id name);

/*
 * AST-resolving functions.
 */

Value* resolveLit(FunBuilder* b, ast::Literal* lit, Id name);
Value* resolveCon(FunBuilder* b, ast::ConExpr* expr, Id name);
Value* resolvePat(FunBuilder* b, Value* pivot, ast::Pat* pat);