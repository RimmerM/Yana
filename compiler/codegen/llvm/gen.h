#pragma once

#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "../../lower/lower.h"
#include "Net/Stream.h"

/*
 * The LLVM exit ramp.
 *
 * This consumes `compiler/lower` and nothing above it, which is the whole reason it is short: the
 * lower IR is already an SSA CFG over five machine types, and LLVM IR is an SSA CFG over machine
 * types plus a type system for memory that the lowering has already spent. Almost every instruction
 * is one builder call, and the ones that are not are the three places where lower says something in
 * bytes that LLVM says in types - see README.md in this directory.
 *
 * The other backends fork elsewhere for reasons of their own: `codegen/js` forks above `lower`
 * because a typed-array heap is not a JavaScript program, and `codegen/x64` consumes the same IR
 * this does because it is the fast path that owns its own register allocation. This one is the
 * optimizing native path, so it hands LLVM the IR and stops.
 */
namespace llvmgen {

// Builds the LLVM form of a lowered module. Reports through `context.diagnostics` for anything the
// target has no meaning for, and returns the module it managed to build regardless - a diagnostic
// plus the surrounding code is more use than nothing.
Ptr<llvm::Module> genModule(llvm::LLVMContext& llvm, Context& context, LowerModule& module);

// Runs LLVM's own verifier over a generated module, reporting what it rejects as diagnostics.
// Nothing downstream is allowed to see a module this returns false for.
bool verifyGenModule(Context& context, llvm::Module& module);

// The module as LLVM assembly.
void printModule(Net::Writer& writer, llvm::Module& module);

/*
 * Native emission.
 *
 * Separated from the generation above because they need different things: everything above works
 * from the IR alone, and everything below needs a target machine, a file system and a linker.
 */

// Gives the module a C-callable `main` that calls the lowered entry point and returns its result as
// the process exit status. The lowered entry keeps its own convention and is renamed out of the way;
// see the comment on the definition for why a wrapper rather than a convention change.
bool addNativeEntry(Context& context, llvm::Module& module, StringId entryName);

// Runs LLVM's default optimization pipeline at the given level (0-3) over the module.
void optimizeModule(Context& context, llvm::Module& module, U32 level);

bool writeIrFile(Context& context, llvm::Module& module, const String& path);
bool writeObjectFile(Context& context, llvm::Module& module, const String& path);

// Links one object file into an executable through the platform's C toolchain driver, which is what
// supplies the startup code and the C library the runtime's syscalls stand beside.
bool linkExecutable(Context& context, const String& objectPath, const String& outputPath);

} // namespace llvmgen
