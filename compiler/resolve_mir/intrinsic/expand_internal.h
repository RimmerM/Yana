#pragma once

#include "expand.h"

namespace mir {

/*
 * The emitters, one declaration per `op.def` row, grouped into files by family.
 *
 * Here rather than in `expand.h` because they are not the interface: what the resolver calls is
 * `expandIntrinsic`, which is one switch over the role enum in `expand.cpp`. Splitting them this way
 * is what keeps that switch a *roster* - a family's file holds its emitters and nothing else, and
 * adding a family adds a file and a set of arms rather than growing one of them.
 *
 * Every one of them takes the same four things and nothing more: the values the ordinary call path
 * prepared, the callee whose declared signature they fill, the substitution the call bound, and
 * where it was written. An emitter that wanted anything else would be one reaching past what a call
 * site is - see the note on `expandIntrinsic`.
 */

// expand_pointer.cpp - the raw pointer, `doc/spec/native.md`.
Node* expandAddressOf(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);
// Both borrow roles: which one is `callee.result`'s to say, so there is one emitter.
Node* expandPointerBorrow(Builder& builder, Function& callee, Node* const* arguments,
                          const Substitution& with, LocationId source);
Node* expandPointerAdd(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);
Node* expandPointerSub(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);
Node* expandPointerDifference(Builder& builder, Function& callee, Node* const* arguments,
                              const Substitution& with, LocationId source);
Node* expandPointerLoad(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);
Node* expandPointerStore(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source);

// `traits()` - the one emitter with no arguments at all: its type comes from the result position.
Node* expandTypeTraits(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);


// expand_native.cpp - the operations a machine has and the IR does not, `native.def`.
Node* expandBlockCopy(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);
Node* expandSetMemory(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);

// The one emitter with an arity, because there are seven declarations and one operation.
Node* expandSyscall(Builder& builder, Function& callee, Node* const* arguments, Size count,
                    const Substitution& with, LocationId source);

} // namespace mir
