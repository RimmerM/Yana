#pragma once

#include "module.h"
#include "../lower/lower.h"

Ptr<LowerModule> lowerModule(Context& context, Module& module);
