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
Node* expandPointerLoad(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);
Node* expandPointerStore(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source);

} // namespace mir
