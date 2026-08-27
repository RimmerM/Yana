#pragma once

#include "../compiler/settings.h"

/*
 * How a program starts, for the two backends that have to agree about it.
 *
 * Both native paths produce an amd64 Linux executable and both therefore write the same four facts:
 * where the kernel left the command line, what the stack has to be aligned to before anything of the
 * program runs, how the process ends, and what the exit status is. `codegen/x64/emit.cpp` writes
 * them as bytes into an image it lays out itself; `codegen/llvm/emit.cpp` writes them as a few lines
 * of assembly in the module it hands to LLVM. The *numbers* are here so that a change to one is not
 * a divergence from the other.
 *
 * Which globals the command line lands in is not here - that is `compiler/compiler/builtin.h`, and
 * it is a question about the library rather than about the machine.
 *
 * Nothing in this file is about a code generator's own IR, which is why it is a header of constants
 * and not an interface. Neither backend can call the other's emitter: one writes machine code and
 * the other writes text for an assembler.
 */

// The system call that ends a process, and the register the local conventions return a status in -
// see kComplexResults in constraint.cpp. Both are amd64 Linux facts rather than choices.
static constexpr U32 kSysExitGroup = 231;

/*
 * What the stack is aligned to before the program is entered.
 *
 * `kMaxVectorBytes` rather than the sixteen the psABI asks for: a frame aligned for a 64-byte vector
 * is aligned for every narrower one, sixteen divides it, and the at most 63 bytes it costs is a
 * stack that is megabytes. See genProcessEntry, which is where the rest of that argument - including
 * why nothing has to be *reserved* above the outermost frame - is written down.
 */
constexpr U32 kEntryStackAlignment = kMaxVectorBytes;

static_assert(kEntryStackAlignment % 16 == 0 && kEntryStackAlignment <= 128,
              "the entry stub aligns with a sign-extended imm8, and the convention below it wants sixteen");
