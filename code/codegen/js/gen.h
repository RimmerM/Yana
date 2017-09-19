#pragma once

#include <ostream>
#include "ast.h"
#include "../../resolve/module.h"

namespace js {
    File* genModule(Context* context, Module* module);
    void formatFile(Context& context, std::ostream& to, File* file, bool minify);
}
