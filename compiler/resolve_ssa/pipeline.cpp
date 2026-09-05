#include "pipeline.h"
#include "verify.h"
#include "../resolve_mir/verify.h"
#include "lower/gen.h"

namespace ssa {

bool Pipeline::resolve(ast::ModuleGroup& root, ast::ModuleProvider* provider) {
    program = mir::resolveProgram(context, root, provider);
    return program && context.diagnostics.errorCount() == 0;
}

bool Pipeline::resolve(ast::Module& root, ast::ModuleProvider* provider) {
    program = mir::resolveProgram(context, root, provider);
    return program && context.diagnostics.errorCount() == 0;
}

bool Pipeline::convert() {
    if(!program) return false;

    // The unconditional boundary check §12.1 asks for. `verifyProgram` also runs at each seal in an
    // assertion build; this is the one that runs in every build, because converting IR that has not
    // been checked is how a representation failure becomes a backend failure.
    if(!mir::verifyProgram(context, *program)) return false;
    if(!convertProgram(context, *program, converted)) return false;

    return verifyProgram(context, *program, converted);
}

Ptr<LowerModule> Pipeline::lower() {
    if(!program) return nullptr;
    return lowerProgram(context, *program, converted);
}

void Pipeline::releaseMir() {
    if(program) program->releaseMir();
}

} // namespace ssa
