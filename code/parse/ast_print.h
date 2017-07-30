#pragma once

#include "ast.h"
#include <ostream>

void printModule(std::ostream& stream, Context& context, const Module& module);
void printDecl(std::ostream& stream, Context& context, const Decl& decl);
void printExpr(std::ostream& stream, Context& context, const Expr& expr);