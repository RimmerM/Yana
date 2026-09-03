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
 * That restriction is also what makes the attribute a *directive*. `@inline` is a hint because the
 * pass it asks may decline; this is refused **at the declaration** when the body is not of the shape,
 * so a declaration that carries it either expands at every call or does not compile. `@inline(always)`
 * was reserved rather than accepted for exactly as long as nothing could keep that promise - see
 * readInlineAttribute, which is where the two are told apart.
 *
 * ## Why the resolver and not the optimizer
 *
 * Because the alternative is a call the optimizer has to take back out, and the optimizer is
 * optional. `-no-ir-opt` is a supported way to build, the resolve fixtures read the IR the resolver
 * produced, and a `null()` that is one `bitcast` after optimization and a call before it is a
 * function whose cost depends on a flag. The declarations that want this are the ones whose *whole
 * body* is the operation - `null`, `isNull`, `sizeOf`, the host wrappers in `Native/Host.js.yana` -
 * and for those a call is not a cost to be recovered later; it is a thing that should never have
 * been built.
 *
 * It is what let `null` and `isNull` stop being intrinsics. Those two had C++ emitters for cost
 * alone, and the compiler paid for them by holding an opinion about the target's address space -
 * see native.cpp. The rule here is about a *shape* rather than about a name, so a program's own
 * one-line accessors get the same treatment as the library's.
 */

/*
 * Whether this body may be expanded at its call sites, reported at the declaration if not.
 *
 * Called once, when the body has just been resolved. The answer goes on `Function::alwaysInline`,
 * which is cleared when it is no - so a refused declaration is compiled and called as an ordinary
 * function, on top of the diagnostic, rather than leaving call sites to fail one at a time.
 */
void checkAlwaysInline(Module& module, Function& function);

/*
 * The expansion, at one call site, with the arguments already converted to the declared types.
 *
 * The callee is settled - a plain function, or the specialization the call's type arguments asked
 * for - because a generic body's class calls are `gencall`s and what they become is what decides
 * whether it grafts at all. See `isGraftable`, where the two strictnesses are argued, and
 * emitGenericCall, which is what routes a generic one through `instantiateFunction` first rather
 * than erasing it.
 *
 * Answers `Nothing()` where the body is not one of these, so the caller falls through to the
 * ordinary call. A unit-valued body expands to `Just(nullptr)` - which is why this is a `Maybe` of a
 * pointer rather than a pointer.
 */
Maybe<ModulePtr<Value>> foldTrivialBody(ExprResolver& resolver, ModulePtr<Function> callee,
                                        Buffer<ModulePtr<Value>> args, StringId resultName);
