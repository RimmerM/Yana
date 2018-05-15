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
inline void error(FunBuilder* b, StringBuffer message, const Node* node, T&&... p) {
    b->context.diagnostics.error(message, node, noSource, forward<T>(p)...);
}

Value* implicitConvert(FunBuilder* b, Value* v, Type* targetType, bool isConstruct, bool require);
bool alwaysTrue(Value* v);
bool alwaysFalse(Value* v);

Value* findVar(FunBuilder* b, Id name);
Value* useValue(FunBuilder* b, Value* value, bool asRV);
Value* getField(FunBuilder* b, Value* value, Id name, U32 field, Type* type);
Value* getNestedField(FunBuilder* b, Value* value, Id name, U32 firstField, U32 secondField, Type* type);

Value* resolveStaticCall(FunBuilder* b, Id funName, Value* firstArg, List<ast::TupArg>* argList, Id name);
Value* genStaticCall(FunBuilder* b, Id funName, Value** args, U32 count, Id name);
Value* resolveDynCall(FunBuilder* b, Value* callee, List<ast::TupArg>* argList, Id name);

/*
 * AST-resolving functions.
 */

Value* resolveLit(FunBuilder* b, ast::Literal* lit, Id name);
Value* resolveCon(FunBuilder* b, ast::ConExpr* expr, Id name);

// The matching context is used to determine whether a certain pattern will always succeed,
// based on the other patterns that have been checked in the same context.
// For example, matching a certain constructor will always succeed if every other possible constructor
// has been tested already (using an always-succeeding pattern, of course).
// The context is recursive in order to correctly handle cases like the following:
//   match v:
//     Just(Just) -> True
//     Just(Nothing) -> True
//     Nothing -> True
struct MatchContext {
    // Contexts for nested children of this pattern, such as a TupPat.
    MatchContext* children = nullptr;
    MatchContext* next = nullptr;

    // The constructors that have been unconditionally tested for the type at this position.
    bool* conChecked = nullptr;

    // The number of constructors the type at this position has.
    U16 conCount = 0;

    // The number of true-values in the conChecked array.
    U16 checkedCount = 0;

    // The index of this child context within the parent context.
    U32 childIndex = 0;
};

// Generates pattern matching code on the provided value.
// `onFail` is the block the generated code will jump to if it fails.
// After returning, b->block contains the block that will be executed if the match succeeds -
// this can access any symbols defined while matching.
// Returns 0 if the match may succeed, 1 if the match always succeeds, -1 if the match never succeeds.
int resolvePat(FunBuilder* b, MatchContext** match, Block* onFail, Value* pivot, ast::Pat* pat);
