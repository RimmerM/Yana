#pragma once

#include "module.h"

/*
 * The `Atomic` module's compiler half - Analysis-Atomics.md §3.
 *
 * Every operation the module declares is a machine instruction, so every one of them is an intrinsic
 * hook rather than a body: `load(x, LoadAcquire)` has to *be* an atomic load in an unoptimized build
 * and not a call that an optimizer might later see through, for the reason `Num(Int).+` is an
 * intrinsic - reaching it must cost nothing without a pass having run.
 *
 * Native only. On a JS build `lib/Atomic/Atomic.native.yana` is not read at all, so the module has
 * no declarations for this to attach to and `definePreludeAtomic` returns immediately.
 */
void definePreludeAtomic(Program& program, Module& atomic);

// `Native.atomicAt`, whose declaration is Native's and whose body is this module's - see the
// definition. Called only where the atomic module exists, since the signature names `Atomic(a)`.
void definePreludeNativeAtomic(Program& program, Module& native);
