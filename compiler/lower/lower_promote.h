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
 * place for four separate reasons. `isDirectType` was target-independent by contract (it was
 * `compiler/resolve`'s, the stage deleted in J173, and resolve_ssa/lower/manifest.cpp makes the
 * same statement now), and whether a record fits one register is a *target's* answer that JS
 * declines - so deciding it there would make the set of accepted programs depend on which backend
 * was running. It is also the language's budget rather than a machine's: `kMaxPackBits` is 64, and
 * a target with 32-bit registers would have to decline records the resolver had already promised
 * were registers. A direct value has no address, so `&f.a` on such a record would have nowhere to
 * point, and demoting whenever an address is taken is the demand analysis - which is this pass,
 * arrived at from the other end. And it decides the calling convention, so a record's ABI would
 * fork by target.
 *
 * None of that applies to a transformation over lowered IR. It cannot change which programs compile,
 * what any diagnostic says, or how anything is passed; it can only remove memory traffic that the
 * translation introduced. It also generalizes past the case that prompted it - a mutable local, a
 * value carried across a branch and a scalar record read out of a word are all the same shape here.
 *
 * ## What it does not do
 *
 * Whole slots only - apart from an integer load or store of the slot's low bytes, which is a mask
 * or a merge of its register (`isNarrowAccess`, J152). A slot whose address has arithmetic done to
 * it - which is every aggregate whose fields are at different offsets - is left alone, because
 * splitting one is a different transformation (`splitAggregateSlots` in lower_split.h, which runs
 * just above this and hands it the cells it cut). What this catches is the slot that is loaded and
 * stored as one value and nothing else: a scalar record, an integer local, a pointer, and the
 * temporary `materializeScalar` allocates to hand a scalar record's bits to something that wanted
 * an address. A zero fill of the whole slot counts as a store of zero - it is how lowering clears a
 * slot with a padded word (`zeroStorage`), and it names no register, so like a copy it types
 * nothing.
 *
 * ## A slot only copies name - J183
 *
 * Neither a copy nor a fill says what register a slot is, so a slot whose whole traffic is those
 * two has no type to promote with - and that is a common slot: the temporary a swap goes through
 * (`t = a; a = b; b = t`), a constant copied into place through one, and a value read out of memory
 * that may overlap where it is going, which is exactly the temporary `forwardCopyDestinations` must
 * not remove. Declined, each was a round trip through the frame: a vector load and store into the
 * slot and a second pair out of it, where one load and one store is the whole of the work.
 *
 * So such a slot takes a register after all, and the one it takes is chosen rather than read: a
 * promoted slot it is copied to or from, where that register holds its width, and otherwise an
 * integer at one to eight bytes and an integer vector at sixteen, or thirty-two where
 * `LowerModule::vectorBytes` says the target has such a register. **Holding it in a register is
 * sound even where the two ends of the round trip overlap**, which is the one argument this needs:
 * the copy in becomes a load of the whole value and the copy out a store of it, so every byte is
 * read before any is written - the order the temporary was there to keep.
 *
 * The local backend only, as `splitBranchConditions` is (`LowerModule::consumer`). LLVM's own
 * `memcpy` handling reads a copy better than the load and store this hands it: over the corpus the
 * rule was +0.31% of `.text` under `-backend llvm -opt 3` and -0.15% locally, where it also took
 * 2,182 of `test/base`'s 8,438 copies out of the lowered IR. `doc/progress` J183 has the numbers.
 *
 * What is still declined is a width no register holds: 7, 15 and 20 bytes are values whose size
 * is not their stride, and a wider transfer would reach bytes that belong to their container.
 *
 * It leaves its own litter behind - the byte count of an allocation that is gone, the mask a load
 * turned out not to need - and does not clean it up. `removeDeadValues` in lower_fold.h does, for
 * this pass and for folding alike, and the caller runs it afterwards: this one returns early where
 * there is nothing to promote, and the constants folding orphans are there either way.
 */
// How many bits a value of this type occupies in a register, and therefore whether a load narrower
// than the slot's register truncated something on the way out of memory. A vector occupies its own
// width, which is the whole register it lives in - there is no narrower load of one.
inline U32 registerBits(LowerType type) {
    if(isVectorLike(type)) return type.byteWidth() * 8;
    return type == LowerType::Int32 || type == LowerType::Float32 ? 32 : 64;
}

/*
 * Whether a slot of this many bytes is one a register could hold - §34.4 of test/bench/findings.md.
 *
 * The four scalar widths, and the three a vector register has. **A vector width is safe to accept
 * without asking the target**, which is what makes this a portable question after all: a 32-byte
 * vector *type* exists only in a program compiled for a machine with a 32-byte register, because
 * `targetVectorBytes` is what decided the lane count when the type was made. A target that has no
 * such register has no value of the type either. The one type this tier makes up rather than reads -
 * a slot only copies name, above - asks `LowerModule::vectorBytes` instead, for exactly that reason.
 *
 * Until this said so, every `let &acc = zero() :: Vec(a)` in the language was a stack slot loaded
 * and stored once per chunk - which is not only the traffic but a loop-carried *memory* dependency,
 * and it is why a hand-written vector loop with no call in it measured slower than the same loop
 * behind one.
 */
inline bool holdableWidth(U32 width, LowerType type) {
    if(isVectorLike(type)) return width == 16 || width == 32 || width == 64;
    return width == 1 || width == 2 || width == 4 || width == 8;
}

/*
 * Whether promotion would take a slot of this width holding a value of this type at all - the whole
 * of what `collectSlots` asks about the two once it has a type to ask with.
 *
 * Here rather than inside the pass because `splitAggregateSlots` has to know it: the pieces it cuts
 * an aggregate into are worth cutting exactly when promotion can then hold them, and a field it
 * moves as a typed load and store rather than as a copy is one it has decided the answer is yes for.
 * A rule stated twice would drift, and the way it would drift is a split that buys nothing.
 */
inline bool promotableSlot(U32 width, LowerType type) {
    if(!holdableWidth(width, type)) return false;

    // A float or a pointer read out of storage narrower than itself is not something promotion
    // reproduces - and not something anything emits, since both are stored whole. A vector is the
    // same rule: `registerBits` answers its own width.
    return isInt(type) || width * 8 == registerBits(type);
}

void promoteStackSlots(LowerBase base, LowerFunction& fun);
