#pragma once

#include "module.h"

Module* coreModule(Context* context);
Module* nativeModule(Context* context, Module* core);