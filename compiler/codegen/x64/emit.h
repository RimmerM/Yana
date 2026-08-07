#pragma once

#include "gen.h"
#include "../../compiler/context.h"

/*
 * The whole-program exit ramp for the local backend.
 *
 * Everything else in this directory is about one function: `transformFunction` normalizes it,
 * `allocateRegisters` places its values, `genFunction` writes its bytes. What is left is the three
 * things only a *program* has - the order the functions and globals are laid out in, the process
 * entry point that calls the first of them, and the file the result is written to - and this is
 * where they are said.
 *
 * This is the path that produces an executable with no assembler, no linker and no LLVM. It can be,
 * because nothing outside the image is referenced: the runtime is written in Yana over raw system
 * calls, so once every address inside the image is known there is nothing left for anyone else to
 * resolve. See the header comment on codegen/elf/elf.h.
 */

// Compiles `module` to an executable file at `path` - code generation, layout, relocation and the
// container. Reports through `context.diagnostics` and returns false if anything refused; the file
// is not created at all unless the whole program was generated.
bool genX64Executable(Context& context, LowerModule& module, const String& path);
