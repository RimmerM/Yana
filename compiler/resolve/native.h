#pragma once

#include "module.h"

/*
 * The Native module.
 *
 * Native is what Design.md's "Pointers" and "Interfacing the OS" describe: raw pointers, the
 * memory and system-call primitives, the heap the runtime allocates from, the platform's system
 * calls, the host's array and string, and what a native `String` is made of. It is deliberately
 * unsafe - a raw pointer carries no lifetime, no exclusivity and no promise that what it points
 * at is initialized - which is why it is a module a program has to import by name rather than
 * something Core makes available everywhere. The fixed-width integer family is Core's, not this
 * module's: naming a width is not an unsafe act, and on JS it is how a record asks to be packed.
 *
 * It is built next to Core and the same way: declarations read from `lib/Native/`, with the
 * compiler supplying only what the language cannot say about itself.
 *
 * **Core imports Native and Native implicitly imports Core**, which is a cycle and is the case
 * Analysis-Modules.md §2.2 exists for: an array is built out of raw pointers and the heap, and a
 * pointer is an `Int` away from an address. The two are one strongly connected component and are
 * resolved as one - see `definePrelude`.
 *
 * **There are two platform files and each target compiles one of them.** `Linux.x64.yana` is the
 * native one - the system call numbers and the memory mapping `allocateHeap` is built on - and
 * `Host.js.yana` is the JS one, the host's array and string. **The selection is in the name**, which
 * is Analysis-Modules.md §2.5: a file whose name carries a target this build is not is not read at
 * all, so neither declares anything on the other's target and neither can forget to say so. Most of
 * the module is split the same way - `Heap.native.yana`, `Memory.native.yana`, `Run.native.yana`,
 * `Process.native.yana` - because a heap, a block copy and a process are things only one of the two
 * targets has.
 *
 * They are files of this module rather than modules of their own, which is what makes the mutual
 * visibility with the rest of Native simply what a module is, rather than an import pushed in by
 * hand. `Linux.x64.yana` also says which *machine*: the call numbers are the x86-64 table and every
 * one of them differs on arm64, which is the second axis §4.2.1 asked for.
 *
 * Nothing downstream distinguishes them by *name*: what the JS emitter drops is what it cannot
 * express plus what stops being reachable once that is gone - see `excludeFunctions` and the call to
 * `markProgramReachable` beside it. A module-name test used to stand in for that and got the answer
 * right for the wrong reason.
 */
void definePreludeNative(Program& program, Module& native);

// Native's `Run(a)`, `Flat(a)` and `StringData`, recorded once its files have been read and before
// any signature that writes `[T]` is resolved - the middle of the prelude's three hooks.
void definePreludeNativeTypes(Program& program, Module& native);

// What a native string is made of, and the three reinterpretations that hand it out. In Native
// rather than in Core because forging a `String` out of unvalidated bytes should take an import
// that says so, and `import Native` is the one visible unsafe act.
void definePreludeNativeText(Program& program, Module& native);
