#pragma once

#include <ostream>
#include "ast.h"

void printModule(std::ostream& stream, Context& context, const ast::Module& module);
void printDecl(std::ostream& stream, Context& context, const ast::Decl& decl);
void printExpr(std::ostream& stream, Context& context, const ast::Expr& expr);