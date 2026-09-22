#pragma once

#include "repr.h"

/*
 * The declaration-level calling convention - Analysis-IR-JS.md §3.7.
 *
 * One `JsAbi` per emitted declaration, frozen before any body is converted, and computed from the
 * declaration alone. Never from a call site: direct, indirect and recursive calls have to agree, and
 * two predicates that each decide at their own end is how they come to agree on the same wrong
 * answer. The verifier compares a call's arguments against the callee's plan, which is what turns a
 * disagreement into a report rather than into a program that returns the wrong number.
 *
 * `flattenArguments` is the native pipeline's (§3.7.1), so a JavaScript compilation sees the
 * declarations as written and this is a pure function of them. What is worth flattening *here* is a
 * different set - the erased pair, the vector lanes, the narrow reference - and none of it is that
 * pass's, which is the whole reason it moved.
 */
namespace jsir {

/*
 * The plan for one declaration, computed and memoized on first ask.
 *
 * `where` is only used for the diagnostic a representation this tier has no answer for produces, so
 * a planning walk over every reachable declaration reports at the declaration and a call reports at
 * the call.
 */
JsAbiId jsAbiOf(JsTarget& target, DeclPtr<mir::Function> function, LocationId where);

/*
 * The plan for one function **type** - what a call through a value is held to.
 *
 * §3.7's "an indirect call carries a `JsAbiId` or a signature whose lookup yields it", and the
 * signature is the type rather than `InstCall::callee`: that handle is the declaration the call was
 * *checked* against, and for a call through a value the conversion leaves it null - the two step
 * sites in `resolve_ssa/convert.cpp` write `through.target = nullptr` - so a plan read out of it
 * would be a plan read out of nothing.
 *
 * `Type::Iter` answers its `step`, which is §3.2.1's decision that an erased iterator needs no
 * family of its own: the pair is the producer's step shim and the state record, and the signature it
 * is called across is a `Fun`. `takesEnv` is always true here, because every call through a pair
 * passes the environment word first and a named function that does not take one reaches the pair
 * through the thunk `assemble.cpp` writes.
 *
 * Answers an invalid id for a type this target has no plan for - a raw `@convention` code pointer,
 * whose call shape is the machine's - and the caller reports at its own location.
 */
JsAbiId jsAbiOfType(JsTarget& target, DeclPtr<mir::Type> type, LocationId where);

/*
 * The plan a **type descriptor's** teardown cell is called across - one erased value in, nothing
 * back.
 *
 * One plan for the whole program rather than one per type, which is the point of it: a body compiled
 * once drops a value of a type it cannot see, so the signature it calls across is the one every
 * descriptor's cell was written to fit. Interned on `JsTarget` so that the verifier compares a call
 * against the same object the filler used.
 */
JsAbiId jsErasedDropAbi(JsTarget& target);

/*
 * The plan a **teardown body of its own** is entered across - one value in, nothing back.
 *
 * Two of those exist and neither is a declaration: a lambda's environment teardown, which takes the
 * word the pair holds, and a type's `$td$`, which takes the value. Both used to build a `JsAbi` at
 * the site and push it, so a program with two hundred per-type teardowns carried two hundred plans
 * nothing interned and a second ask for one type produced a second plan - which is the one thing a
 * frozen ABI table exists not to allow, since the verifier compares a call against an *identity*.
 * Keyed on the position's passing, because that is the whole of what varies between them.
 */
JsAbiId jsTeardownBodyAbi(JsTarget& target, const JsPassing& value);

/*
 * Every reachable declaration's ABI, and every reachable global's representation, before the first
 * body - §5.1's program planning.
 *
 * Eagerly rather than on demand, because the point is that the tables are *stable*: a plan computed
 * while a call site is being emitted is a plan that can be computed twice, and the second time is
 * the one that disagrees. It also means a representation this target has no answer for is reported
 * once at the declaration rather than once per call.
 */
bool planProgram(JsTarget& target);

} // namespace jsir
