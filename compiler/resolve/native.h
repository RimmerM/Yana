#pragma once

#include "module.h"

/*
 * The Native package.
 *
 * Native is what Design.md's "Pointers" and "Interfacing the OS" describe: raw pointers, the
 * memory and system-call primitives, and the heap the runtime allocates from. It is deliberately
 * unsafe - a raw pointer carries no lifetime, no exclusivity and no promise that what it points
 * at is initialized - which is why it is a module a program has to import by name rather than
 * something Core makes available everywhere. The fixed-width integer family is Core's, not this
 * module's: naming a width is not an unsafe act, and on JS it is how a record asks to be packed.
 *
 * It is built next to Core and the same way: declarations read from `lib/Native.yana`, with the
 * compiler supplying only what the language cannot say about itself. Unlike
 * Core it is not implicitly imported, so a program that never writes `import Native` cannot
 * reach any of it.
 *
 * `Native.Linux` is the platform half - the system call numbers and the memory mapping that
 * `allocateHeap` is built on. The two are mutually visible on purpose: Native.Linux is written
 * in terms of Native's pointers and syscall intrinsics, and Native's heap is written in terms of
 * Native.Linux's mapMemory. Which platform module that is will eventually be a target decision;
 * today amd64 Linux is the only backend, so it is the only one built.
 */
void defineNative(Program& program);

// `NativeText` - what a native string is made of, and the three reinterpretations that hand it
// out. A module of its own because it names `Array(U8)` and so must sit above Collections, while
// staying out of the implicit imports - see Program::nativeText.
void defineNativeText(Program& program);
