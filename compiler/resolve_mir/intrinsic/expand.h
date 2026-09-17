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

/*
 * Whether a role is one this stage builds a node for, or one whose *declaration's own body* is the
 * implementation and every call reaches as a call - `providedExpands` one tier over, and the same
 * question.
 *
 * **A role is not a promise that the compiler does the work.** It says the compiler is entitled to,
 * and the shipping stage takes that entitlement where it has something to say that the language
 * cannot: `newRun` is placed by an escape analysis this stage has not got, so what the shipping
 * stage expands into an allocation with a count, this stage reaches as the ordinary library body
 * beside the declaration - which allocates from the heap and says so in the tag.
 *
 * That is still the arrangement, and this stage now says the second half somewhere else. J44 split
 * the declaration: `runSlots` asks for the bytes, `runOver` builds the descriptor over them and
 * takes the tag as an argument, and `resolve_ssa/opt/place.cpp` replaces the *request* where the
 * frame can keep it. So the placement did not move up to a role - it moved down to a tier that can
 * see the caller, which is where the question was answerable all along. The body stays what a call
 * that is not replaced reaches, and stays dead in the shipping stage.
 *
 * Asked here rather than by a `null` out of `expandIntrinsic`, because the two answers are
 * different: an emitter returning null failed and has reported, and this is a call that has not
 * failed at all.
 */
bool opExpands(intrinsic::Op op);

/*
 * expand_provided.cpp - the members of the instances the compiler generates, `provided.def`.
 *
 * Not `Op` roles: a provided member is reached through instance selection rather than through a
 * declaration of its own, so what identifies one is the row and the member index. `head` is the
 * instance's own argument list - the `I32` of `Num(I32)`, the `(I32, I64)` of `Widen(I32, I64)` -
 * which the arms that build a constant or state a result type need and the others read off their
 * operands. The whole list rather than its first entry, because a conversion's answer is its
 * *second*: nothing about `widen`'s argument says which rung it is.
 */
Node* expandProvided(Builder& builder, intrinsic::Provided role, U16 member,
                     const DeclPtr<Type>* head, Size heads, Node* const* arguments, Size count,
                     LocationId source);

// Whether a member is one node - `expandProvided` answers it - or a body of its own, which
// `Resolver::provideBodies` writes and every call reaches as a call.
bool providedExpands(intrinsic::Provided role, U16 member);

// The `Ord` member that is a body rather than a node - `compare`, by `known.def`'s position.
constexpr U16 kOrdCompare = 4;

// `Enum`'s four members, by `known.def`'s position: the tag read, the range test, the constructor's
// own word and the word read back. Three of the four are graphs rather than nodes - see
// `providedExpands`, and `Resolver::provideBodies`, which writes each.
constexpr U16 kEnumValueOf = 0;
constexpr U16 kEnumFromValue = 1;
constexpr U16 kEnumNameOf = 2;
constexpr U16 kEnumFromName = 3;

// `Show`'s two, by `known.def`'s position - the class's own note calls the order part of the claim.
// Both are bodies rather than nodes, so neither ever reaches `expandProvided`.
constexpr U16 kShowShow = 0;
constexpr U16 kShowBound = 1;

// A written number as a constant of one type - see the definition, which is the one place a
// literal's bits are decided.
Constant foldLiteral(DeclBase base, DeclPtr<Type> type, U64 bits, double value);

// Whether `provided.def`'s row generates an instance at this type - the family's membership, asked
// where the instance table cannot be: a constant position is resolved before the instances exist.
bool providedFor(Program& program, intrinsic::Provided role, DeclPtr<Type> type);

} // namespace mir
