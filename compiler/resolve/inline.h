#pragma once

#include "expr.h"

/*
 * `@inline(always)` - a body expanded where it is called, rather than called and inlined later.
 *
 * ## What this is not
 *
 * It is not the inliner, run earlier. compiler/opt's inliner grafts an arbitrary callee: it splices
 * blocks, rewrites the returns into a join, re-roots every place at the caller's storage, carries the
 * ownership state across and weighs the whole thing against a budget. None of that is here, and the
 * reason is the restriction rather than an omission - **a body this expands has no control flow, no
 * storage and no ownership**, so each of those jobs is the empty one. What is left is copying a run
 * of pure computation into the block that is already current, which is `cloneComputation` in clone.h
 * and is shared with the two copiers that already existed.
 *
 * ## Why the resolver, and what happens when the shape does not hold
 *
 * The resolver is where a body of *this* shape is spent, because the alternative is a call the
 * optimizer has to take back out and the optimizer is optional. `-no-ir-opt` is a supported way to
 * build, the resolve fixtures read the IR the resolver produced, and a `null()` that is one
 * `bitcast` after optimization and a call before it is a function whose cost depends on a flag. The
 * declarations this was written for are the ones whose *whole body* is the operation - `null`,
 * `isNull`, `sizeOf`, the host wrappers in `Native/Host.js.yana`.
 *
 * It is what let `null` and `isNull` stop being intrinsics. Those two had C++ emitters for cost
 * alone, and the compiler paid for them by holding an opinion about the target's address space -
 * see native.cpp. The rule here is about a *shape* rather than about a name, so a program's own
 * one-line accessors get the same treatment as the library's.
 *
 * **A body that is not of the shape is no longer refused** - inventory D12, 2026-09-06. The graft
 * declines and the call is emitted; `opt_inline.cpp` then admits the callee *unconditionally*,
 * bounded by its ceiling alone and by nothing else in the table. So the attribute means one thing
 * in both resolve stages, which is what lets `lib/` write `Index(Array(a), Size, a)` as a body
 * rather than leaving it to a generator - `compiler/resolve_ssa/opt/inline.cpp` reached the same
 * rule from the other stage's side, where the graft handles branches and declines only at eight
 * expansions deep.
 *
 * The price is that a branching `@inline(always)` body is a *call* under `-no-ir-opt`, where the
 * one-expression form is not. That is the honest reading of the two halves: what the resolver can
 * carry out with no cost model it still carries out itself, and what needs one is weighed by the
 * thing that has one. The only refusal left is a body that calls itself - see `checkAlwaysInline`,
 * where the argument is that no pass can copy a recursive body without copying the call with it.
 */

/*
 * The one thing about an `@inline(always)` body that is still an error: a body that calls itself.
 *
 * Called once, when the body has just been resolved, which is the earliest point a self-call is
 * visible. The answer goes on `Function::alwaysInline`, cleared when it is no - so a refused
 * declaration is compiled and called as an ordinary function, on top of the diagnostic, rather than
 * leaving call sites to fail one at a time.
 *
 * Every other shape the graft cannot take is not asked about here at all, because it is not an error
 * any more: the header above says which pass takes it instead.
 */
void checkAlwaysInline(Module& module, Function& function);

/*
 * The expansion, at one call site, with the arguments already converted to the declared types.
 *
 * The callee is settled - a plain function, or the specialization the call's type arguments asked
 * for - because a generic body's class calls are `gencall`s and what they become is what decides
 * whether it grafts at all. See `isGraftable`, and emitGenericCall, which is what routes a generic
 * one through `instantiateFunction` first rather than erasing it.
 *
 * Answers `Nothing()` where the body is not one of these, so the caller falls through to the
 * ordinary call - which compiler/opt's inliner is then required to remove. A unit-valued body
 * expands to `Just(nullptr)` - which is why this is a `Maybe` of a pointer rather than a pointer.
 */
Maybe<ModulePtr<Value>> foldTrivialBody(ExprResolver& resolver, ModulePtr<Function> callee,
                                        Buffer<ModulePtr<Value>> args, StringId resultName);
