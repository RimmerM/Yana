#pragma once

#include "../compiler/diagnostics.h"
#include "lower.h"

bool validateLowerInst(Diagnostics* diagnostics, LowerBase base, LowerBlock* block, LowerInst* inst, const DominatorTree& dominators);
bool validateLowerBlock(Diagnostics* diagnostics, LowerBase base, LowerFunction* function, LowerBlock* block, const DominatorTree& dominators);
bool validateLowerGlobal(Diagnostics* diagnostics, LowerBase base, LowerGlobal* global);
bool validateLowerFunction(Diagnostics* diagnostics, LowerBase base, LowerFunction* function);
bool validateLowerModule(Diagnostics* diagnostics, LowerModule* module);
