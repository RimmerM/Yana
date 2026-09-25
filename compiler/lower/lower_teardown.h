#pragma once

#include "lower_inst.h"

/*
 * A record's teardown put back in place where the record is about to become registers - J172.
 *
 * A record's drop reaches this tier as one call of the record's own teardown, handed its address
 * (`teardown(Glyphs)`, built by `Lowering::teardownHalves` in `resolve_ssa/lower/erase.cpp`): the
 * SSA optimizer expanded the drops of the records it saw built, and every other one is in storage
 * somebody else filled, where a call per field would reload what one call reads anyway. That is the
 * per-type function by construction - where the type is dropped at enough sites for one body to
 * pay (`Lowering::sharesRecordTeardown`); a rarer one keeps the walk in place. J171 got there the
 * other way round - the walk written out at every drop and `shareTeardownRuns` finding the runs
 * again here - and this file was that pass.
 *
 * **What the call costs is the slot.** A frame slot whose address is handed to a call stays in
 * memory, and `splitAggregateSlots` would otherwise have taken it apart: a record a call returned in
 * registers and the body stored, or a `->` parameter's fields, are exactly the records the lower
 * tier splits and promotes. So where the teardown's subject is such a slot - every other use of its
 * address a load or a store at a constant offset, and not a copy (`containedButFor` says why) - the
 * call is replaced by the teardown's own body, and the slot splits like any other. Where it is not,
 * the call stays and nothing is lost: the slot was in memory anyway.
 *
 * The decision is made by the pass that knows it, from the slot's own uses, in front of the split
 * that spends it. Nothing is recognized: the callee is named by the call, and what is copied is its
 * body. Only a body of one block of loads, address arithmetic and calls is copied - a plain record's
 * walk, which is what `teardownHalves` writes for one; a sum's branches and a box's release stay
 * behind the call they are in.
 */
void expandTeardownCalls(LowerBase base, LowerModule& module, LowerFunction& fun);
