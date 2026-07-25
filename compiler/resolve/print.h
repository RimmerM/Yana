#pragma once

#include "module.h"
#include "Net/Stream.h"

void printModule(Net::Writer& stream, Context& context, Module& module);
void printFunction(Net::Writer& stream, Context& context, Module& module, const Function* decl, StringId forceName = 0);
void printInst(Net::Writer& stream, Context& context, Module& module, const Inst* inst);
void printType(Net::Writer& stream, Context& context, Module& module, const Type* type);
