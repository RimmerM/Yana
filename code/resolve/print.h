#pragma once

#include <ostream>
#include "module.h"

void printModule(std::ostream& stream, Context& context, const Module* module);
void printFunction(std::ostream& stream, Context& context, const Function* decl);
void printInst(std::ostream& stream, Context& context, const Inst* inst);