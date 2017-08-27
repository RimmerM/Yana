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
bool alwaysTrue(Value* v);
bool alwaysFalse(Value* v);

Value* findVar(FunBuilder* b, Id name);
Value* useValue(FunBuilder* b, Value* value, bool asRV);
Value* getField(FunBuilder* b, Value* value, Id name, U32 field, Type* type);

Value* resolveStaticCall(FunBuilder* b, Id funName, Value* firstArg, List<ast::TupArg>* argList, Id name);
Value* genStaticCall(FunBuilder* b, Id funName, Value** args, U32 count, Id name);
Value* resolveDynCall(FunBuilder* b, Value* callee, List<ast::TupArg>* argList, Id name);

/*
 * AST-resolving functions.
 */

Value* resolveLit(FunBuilder* b, ast::Literal* lit, Id name);
Value* resolveCon(FunBuilder* b, ast::ConExpr* expr, Id name);

// Generates pattern matching code on the provided value.
// `onFail` is the block the generated code will jump to if it fails.
// After returning, b->block contains the block that will be executed if the match succeeds -
// this can access any symbols defined while matching.
// Returns 0 if the match may succeed, 1 if the match always succeeds, -1 if the match never succeeds.
int resolvePat(FunBuilder* b, Block* onFail, Value* pivot, ast::Pat* pat);
