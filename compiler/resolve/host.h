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

/*
 * Whether `Array(element)` is a `TypedArray` on this target rather than the plain host array -
 * Implementation-Containers.md §14's second row, and Design-Vector.md §7.3.
 *
 * One rule with two readers, which is why it is here rather than in either of them: `hostFixedCapacity`
 * reads it during resolution to decide whether `reserve` has anything to do, and the JS emitter reads
 * it to decide what `[]` is. If the two disagreed, an array would be grown as one kind and built as
 * the other.
 *
 * **The element set is the primitive numbers of known width**, which is the same set Design-Vector
 * §2.2 gives a vector lane and for the same reason - a `TypedArray` element is a machine number and
 * nothing else. `Long`/`I64`/`U64` are deliberately *out*: `BigInt64Array` holds `BigInt`s, and a
 * `BigInt` lane is no more a lane here than it is in a vector (§7.3), so those arrays stay the host's
 * own and keep whatever this target already represents a wide integer as.
 *
 * `Bool` is out for a different reason, and it is worth stating because `Uint8Array` would hold one
 * perfectly well: a host array of booleans holds `true`/`false` and a `Uint8Array` holds 1/0, so the
 * two would compare and print differently, and this is a representation change rather than a
 * semantics one. §11's bitset is where a `[Bool]` gets its own answer.
 *
 * Answers the constructor's name as well as the question, because the two are one lookup and a caller
 * that had to ask twice is a caller that could get two answers. Empty when the element is not one.
 */
StringView typedArrayFor(GlobalBase global, TypePtr element);

/*
 * Whether a record holding this `%a` may drop its `@host` fields onto the host value's own
 * properties - Implementation-Containers.md §14's elision, and `Field::host` for what the flag
 * claims.
 *
 * The third reader of the rule above, and the reason it is written here rather than in the code
 * generator is the same one: what the two rows *are* is one question, and a container whose count
 * is elided on one row and stored on the other is a container two passes could disagree about.
 *
 *  - a plain host array's `length` is its occupancy, and assigning it **truncates** - which is the
 *    whole of what closing a gap means, so a count field over one is a second copy of a number the
 *    host already keeps;
 *  - a `TypedArray`'s `length` is its fixed capacity, is not assignable at all, and says nothing
 *    about how many of the slots are live. The record keeps its stored field there.
 *
 * **The typed row was tried as a bare array too and is blocked on a reference form, not on a name.**
 * `class V extends Int32Array { constructor(n) { super(n); this.$n = 0; } }` gives the count an
 * in-object slot for eight bytes where the wrapper costs forty (measured under Node 24 at every
 * width from 8 to 64 elements, and an assigned `a.$n = n` costs the full forty - a typed array has
 * no in-object room, so the first named property allocates a store beside it). What stops it is
 * `growJsArray`: that row's growth *replaces* the array, and with the count elided field zero is the
 * whole container - so `self.items = hostGrow(...)` asks a callee to rebind its caller's binding,
 * which an object-is-its-own-reference `&` cannot express. Making `isJsObject` answer no for that row
 * gets the box and the write-back, and then disagrees with the *erased* boundary, where a generic
 * parameter's callee is compiled against `Array(a)` and boxes on one side only.
 *
 * A generic element answers **no**, and that is a deliberate refusal rather than a gap. The row is
 * chosen per element type, so a body compiled without one has no way to know which layout its caller
 * built - and getting that wrong is not a slow value but a wrong one, since the two do not have the
 * same properties. The erased path keeps the stored field, which is the layout that is correct for
 * both rows.
 */
bool hostPropertiesElided(GlobalBase global, TypePtr pointer);

/*
 * The two host operations a built-in container's own intrinsic reaches.
 *
 * Collections' `Index(Array(a))` and `Length(Array(a))` are `hostAt` and `hostLength` in source, and
 * an intrinsic instance (see attachInstanceIntrinsic) has to emit what those declarations emit. That
 * is a fact about the host and not about containers, so it is said here once rather than in the two
 * hooks - a second spelling of `.length` in another file is exactly the kind of agreement this plan
 * exists to remove.
 */
struct ExprResolver;

// `self[index]`, as the place it is - see the element note in host.cpp.
Place hostElementPlace(ExprResolver& resolver, ModulePtr<Value> array, ModulePtr<Value> index);

// `self.length`, at the result type the caller declares.
ModulePtr<Value> emitHostLengthOf(ExprResolver& resolver, ModulePtr<Value> array, TypePtr type,
                                  LocationId source, StringId name);
