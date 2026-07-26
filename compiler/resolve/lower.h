#pragma once

#include "module.h"
#include "../lower/lower.h"

Ptr<LowerModule> lowerProgram(Context& context, Program& program);
