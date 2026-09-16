#pragma once

#include "lower_inst.h"

/*
 * A load that reads back what the instruction above it just wrote, and a store nothing can have read
 * - §16 of test/bench/findings.md, the item about bitfields expressed as several loads, masks and
 * stores rather than one operation.
 *
 * Writing one `@bits` field of a word is a read-modify-write: load the word, clear the field's bits,
 * merge the new ones in, store it back. Writing *two* fields of one word is that twice, and the front
 * end emits it literally - so `resize` in every container reached the backend as
 *
 *   %a = load %p, 4
 *   %b = or (and %a, 0xc0000000), %new
 *   store %p, %b, 4
 *   %c = load %p, 4          ; the word that was just written
 *   %d = or (and %c, 0x3fffffff), 0x40000000
 *   store %p, %d, 4
 *
 * where the second load is the value the first store already has in a register, and the first store
 * is a word nothing between the two can read. `%c` becoming `%b` is what then lets the mask folding
 * in lower_fold.h see the whole chain at once, and what it collapses to is the single `or` and the
 * single store LLVM emits.
 *
 * ## What it is allowed to assume
 *
 * Block-local. A forwarded load has to read through the **same value** its store wrote through,
 * rather than an address that computes the same number - telling `%p` apart from some other pointer
 * is disambiguation over values, the question lower_cse.h declines to ask about loads, and for the
 * same reason.
 *
 * What the walk *does* compare is where each access lands relative to a root: the value reached by
 * walking back through constant `add`s, and the byte range from there. That is arithmetic on
 * literals rather than a question about values, and it answers two things:
 *
 *  - **Whether a write clears a record.** A store to one field of a record leaves a record of another
 *    field of it standing, which is what lets a field written again be recognized at all - it used
 *    to be that every store forgot every record whose allocation it could not prove different.
 *  - **Whether a write replaces a record.** A store, a copy or a fill that covers every byte of an
 *    earlier one nothing read makes that one dead - so a slot zeroed and then overwritten whole by a
 *    copy loses the zeroing, whatever width either was written at.
 *
 * A write of zeroes answers two more things. A load every byte of which a zero fill or a zero store
 * last wrote is a zero, which is what the first read-modify-write of a packed word in a cleared slot
 * reads. And a zero written over bytes a standing zero write already covers is no write at all - a
 * record literal's zero fields in a slot its padded word already cleared.
 *
 * Two roots are only known apart where both are allocations, or where one is an allocation whose
 * address is used for nothing but its own accesses (`addressContained` in lower_store.cpp) - nothing
 * else can point into that, not a parameter and not a call. Everything else that could read or write
 * storage between two accesses is taken as touching every record: a call, an intrinsic, and an
 * access through a root this cannot place.
 *
 * ## The two things a forwarded load is not
 *
 * **A narrower store than the load.** A four-byte store under an eight-byte load leaves the upper
 * half whatever it already was, which is not a value this pass has. The widths have to match.
 *
 * **An extension.** A four-byte load into a `Long` zeroes bits 32 to 63, so the value is the stored
 * one only where the stored one already has them zero. That is `knownZeroBits` (lower_fold.h) rather
 * than a mask emitted here: a mask this pass emits is one the fold behind it would have to remove
 * again, and the shape that needs one - a store of a value wider than the field - is one the front
 * end does not write.
 *
 * ## Where it runs
 *
 * After `eliminateCommonValues`, and that is the whole of why it is not beside `promoteStackSlots`
 * where it otherwise belongs. Two accesses of one field are written as two `add %self, 8`, so "the
 * same pointer" is not true of them until the CSE has made those one value, and a forwarded load
 * compares pointers by identity precisely so that it never has to ask anything harder.
 *
 * What reads the value it forwards is the last `foldFunctionConstants` of the pipeline: the mask
 * folding in lower_fold.h is what turns the forwarded chain into the single merge it always was.
 */
void forwardStoredValues(LowerBase base, LowerModule& module, LowerFunction& fun);
