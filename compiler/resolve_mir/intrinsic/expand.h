#pragma once

#include "../build.h"

namespace mir {

/*
 * What an intrinsic call builds, in place of the call node an ordinary callee would get.
 *
 * ## An intrinsic call is an ordinary call up to the last step
 *
 * This is the whole design, and it is worth stating because the arrangement it replaces did not have
 * it. Name resolution, arity, type inference, constraint discharge and - the one that matters -
 * **argument preparation** all run exactly as they do for a call to a function with a body. What
 * arrives here is `Node*` per argument, already read under the mode `argumentMode` answered for the
 * declared contract, with the loans made and the lineages united.
 *
 * The current resolver's `expandIntrinsic` applies none of the declared conventions and lets each
 * emitter decide, which is how `Atomic.intoValue(->self: Atomic(a))` came to consume nothing: the
 * declaration said the subject was handed over, three emitters applied the handover by hand with a
 * comment saying so, and the fourth did not. Here an emitter *cannot* forget, because there is
 * nothing for it to remember - the values are prepared before it is called, by the same function
 * that prepares an ordinary call's.
 *
 * ## And the shape is already checked
 *
 * `checkRoles` has matched the declaration against `op.def`'s shape before any body is resolved, so
 * an emitter may destructure the positions the row states without testing them: `pointerLoad`'s
 * argument zero *is* a pointer, and its result *is* that pointer's pointee. An emitter that needs
 * something the row does not state is an emitter whose row is missing a term.
 *
 * ## What it does not do
 *
 * It never reports a *program* error. Every diagnostic an intrinsic call can produce - a wrong
 * argument type, an unbound variable, an unsatisfied constraint - is produced by the ordinary call
 * path above it, against the declared signature. What is left here is construction, and a failure
 * would mean the compiler's own table and its own emitter disagree.
 */
Node* expandIntrinsic(Builder& builder, Context& context, intrinsic::Op op, Function& callee,
                      Node* const* arguments, Size count, const Substitution& with,
                      LocationId source);

} // namespace mir
