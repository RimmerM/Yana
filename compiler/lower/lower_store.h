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
 * Block-local, and the address has to be the **same value** rather than an address that computes the
 * same number. A place is an address by this point, so telling `%p` apart from some other pointer is
 * disambiguation over values - the question lower_cse.h declines to ask about loads, and for the same
 * reason. What is left needs no aliasing at all: a load reading the value of the store immediately
 * above it, through the pointer that store was given.
 *
 * So anything that could read or write storage between the two ends it - a call, a copy, a pattern
 * fill, an intrinsic, and a store through any other pointer. The record is a single store rather than
 * a table for exactly that reason: a second store is what clears the first, whether or not the two
 * addresses have anything to do with each other.
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
 * same pointer" is not true of them until the CSE has made those one value - and this pass compares
 * pointers by identity precisely so that it never has to ask anything harder.
 *
 * What reads the value it forwards is the last `foldFunctionConstants` of the pipeline: the mask
 * folding in lower_fold.h is what turns the forwarded chain into the single merge it always was.
 */
void forwardStoredValues(LowerBase base, LowerModule& module, LowerFunction& fun);
