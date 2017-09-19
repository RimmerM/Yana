#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include "gen.h"
#include <llvm/Linker/Linker.h>

llvm::Module* linkModules(llvm::LLVMContext* llvm, Context* context, llvm::Module** modules, U32 count)  {
    if(count == 0) return nullptr;
    if(count == 1) return modules[0];

    llvm::Linker linker(*modules[0]);
    for(U32 i = 1; i < count; i++) {
        linker.linkInModule(std::unique_ptr<llvm::Module>(modules[i]));
    }

    return modules[0];
}
