#pragma once

#include "module.h"

/*
 * The Host package - Implementation-Containers.md §14.1.
 *
 * The back half of an FFI without its front half. §14 needs one thing from a foreign-function
 * interface - a way for a container's implementation to say "call this method on this receiver" -
 * and everything else an FFI is (syntax for naming a host entity, what a `foreign` declaration means
 * to the checker, to the effect walk and to the ownership passes, a marshalling story) is already
 * supplied by machinery that exists: `attachIntrinsic`, plus `@platform`.
 *
 * So this module is a list of declarations with real Yana signatures and no bodies, each with a hook
 * that emits one `InstNative` where it is called. The resolver checks every one of them like any
 * other call, which is what makes the boundary typed rather than unchecked.
 *
 * **Every declaration here is `@platform(js)`**, so on a native build this module is empty and the
 * hooks are not attached. That is what makes the native lowering's arm for a host operation an
 * internal error rather than a translation - `platformEnabled` runs during resolution, so a native
 * build contains no name, no type and no instance that could reach one.
 *
 * It is a module of its own rather than part of Collections for one reason: Collections is
 * implicitly imported everywhere, and `hostPush` is not a name a program should find by writing it.
 * It is not part of `Native` for the opposite reason - `nativeModule` excludes everything called
 * `Native` or `Native.*` from this target by name, before expressibility is even asked, so the one
 * module whose contents are *only* expressible here is the one module that may not live there.
 *
 * **What a `%a` is on this target.** These signatures are written over raw pointers, and on JS a
 * pointer is not an address: the Repr gives it `null` for a zero and the emitter passes it about as
 * an opaque reference, because nothing this target can express ever does arithmetic on one - every
 * function that adds to a pointer, or converts one to an integer, is excluded from the target
 * entirely (see expressibleInJs). So `%a` here means "a reference to storage holding `a`s", and a
 * host array is exactly that. Nothing new had to be invented to name one, and the ownership passes
 * treat a host element exactly as they treat a native one: a place rooted in a raw pointer, outside
 * the ownership graph, which is what makes `Array(a)`'s teardown the authored traversal on both
 * targets rather than two different rules.
 */
void defineHost(Program& program);
