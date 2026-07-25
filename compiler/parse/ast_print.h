#pragma once

#include "ast.h"
#include "Net/Stream.h"

void printModule(Net::Writer& stream, Context& context, ast::ParseBase base, ast::Module& module);
void printDecl(Net::Writer& stream, Context& context, ast::ParseBase base, ast::Decl& decl);
void printExpr(Net::Writer& stream, Context& context, ast::ParseBase base, ast::Expr& expr);
