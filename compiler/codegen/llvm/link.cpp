#include <fstream>
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

void printModules(llvm::LLVMContext* llvm, Context* context, Buffer<Module*> modules, const String& path) {
    std::ofstream llvmFile(std::string(path.text(), path.size()), std::ios_base::out);
    llvm::LLVMContext llvmContext;

    Array<llvm::Module*> llvmModules;
    for(auto module: modules) {
        llvmModules.push(genModule(&llvmContext, context, module));
    }

    auto result = linkModules(&llvmContext, context, llvmModules.pointer(), llvmModules.size());
    llvm::raw_os_ostream stream{llvmFile};
    result->print(stream, nullptr);
}
