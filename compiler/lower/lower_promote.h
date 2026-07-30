#pragma once

#include "lower.h"

/*
 * Stack slots that did not need to be on the stack.
 *
 * Lowering gives every local storage, because that is the only shape resolve's places have: a place
 * is an address plus a path, and even a local that is written once and read once is an `alloca`, a
 * `store` and a `load`. Which of those slots genuinely need memory is not a question resolve can
 * answer - it depends on what the *lowered* form ended up doing with the address, not on what the
 * source said - so it is asked here, over finished IR, where the answer is visible.
 *
 * ## Why here rather than in isDirectType
 *
 * The alternative was to let resolve call a scalar record a register value, and it is the wrong
 * place for four separate reasons. `isDirectType` is target-independent by contract (see
 * resolve/type.h), and whether a record fits one register is a *target's* answer that JS declines -
 * so deciding it there would make the set of accepted programs depend on which backend was running.
 * It is also the language's budget rather than a machine's: `kMaxPackBits` is 64, and a target with
 * 32-bit registers would have to decline records the resolver had already promised were registers.
 * A direct value has no address, so `&f.a` on such a record would have nowhere to point, and
 * demoting whenever an address is taken is the demand analysis - which is this pass, arrived at from
 * the other end. And it decides the calling convention, so a record's ABI would fork by target.
 *
 * None of that applies to a transformation over lowered IR. It cannot change which programs compile,
 * what any diagnostic says, or how anything is passed; it can only remove memory traffic that the
 * translation introduced. It also generalizes past the case that prompted it - a mutable local, a
 * value carried across a branch and a scalar record read out of a word are all the same shape here.
 *
 * ## What it does not do
 *
 * Whole slots only. A slot whose address has arithmetic done to it - which is every aggregate whose
 * fields are at different offsets - is left alone, because splitting one is a different
 * transformation (see `scalarizable` in resolve/lower.cpp, which does exactly that for the shapes it
 * can prove safe). What this catches is the slot that is loaded and stored as one value and nothing
 * else: a scalar record, an integer local, a pointer, and the temporary `materializeScalar` allocates
 * to hand a scalar record's bits to something that wanted an address.
 */
void promoteStackSlots(LowerBase base, LowerFunction& fun);
