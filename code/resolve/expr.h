#pragma once

#include "module.h"

namespace ast {
    struct ConExpr;
}

/*
 * Helper functions for instruction generation.
 */

template<class... T>
inline void error(FunBuilder* b, const char* message, const Node* node, T&&... p) {
    b->context.diagnostics.error(message, node, nullptr, forward<T>(p)...);
}

Value* implicitConvert(FunBuilder* b, Value* v, Type* targetType, bool isConstruct, bool require);

/*
 * AST-resolving functions.
 */

Value* resolveCon(FunBuilder* b, ast::ConExpr* expr, Id name);