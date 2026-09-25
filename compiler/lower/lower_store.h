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
 * A block, and what stood at the end of a block's one predecessor when it is entered (J169) - never
 * a join, and never a back edge; `entryOf` in lower_store.cpp says why the join was measured out. A
 * forwarded load has to read the place its store wrote: the **same pointer value**, or two that walk
 * to one root at one offset (J168), rather than two addresses that merely compute the same number -
 * telling `%p` apart from some other pointer is disambiguation over values, the question lower_cse.h
 * declines to ask about loads, and for the same reason.
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

/*
 * Where an access lands, as far as this pass can name it: a root value, a constant byte offset from
 * it, and how many bytes the access reaches.
 *
 * `add %slot, 8` is how every field past the first is addressed, so the root is found by walking
 * through the constant offsets that name a field. The walk stops at *any* value - an `Alloca`, a
 * parameter, a call's result - and what it reached is the root either way: two accesses under one
 * root value are the same object, and their offsets can be compared as numbers. Two under different
 * roots are only known apart where both roots are allocations.
 *
 * It used to be the allocation alone, and two fields of one record aliased as far as this pass was
 * concerned. That made every store to a record forget every other store to it, so a record zeroed
 * field by field and then written again - which is what a fresh value built in a frame slot is - kept
 * the whole first run.
 */
constexpr U64 kUnbounded = maxLimit<U64>;

struct LowerPlace {
    // Null where the walk could not finish - a place that may be anywhere.
    LowerValue* root = nullptr;
    bool allocation = false;

    // An allocation whose address is only ever used to reach its own bytes - see `addressContained`.
    bool contained = false;

    U64 offset = 0;
    U64 bytes = 0;
};

// Shared with the loop passes in lower_licm.cpp, which ask the same aliasing question of a whole
// loop that this pass asks of a block - J158. One statement of it rather than two. `LowerPlace`
// rather than `Place` since it left the anonymous namespace: `compiler/resolve`'s inst.h (deleted
// in J173) had a `Place` of its own, and two layouts of one name are one `SmallArray<Place>`
// instantiation under LTO - which is how a first build of this crashed in `-O3 -flto` alone.
bool addressContained(LowerBase base, LowerValue* address, U32 depth);
LowerPlace placeOf(LowerBase base, HashMap<U32, U8>& contained, LowerPtr<LowerValue> pointer, U64 bytes);

/*
 * The same walk with no `contained` table, so every place it answers has `contained` false - the
 * cautious answer. For a reader that asks only where an access lands and whether its root is an
 * allocation (`allocationBase` in lower_forward.cpp), and never whether that root escaped.
 *
 * **This is the one walk from an address up to its root in the lower tier** - J163. The loop passes
 * had a second (lower_licm.cpp's `addressOf`, depth four, any immediate taken as an offset) and
 * lower_forward.cpp a third (`allocationBase`, depth eight, likewise); the first is the review r5/p3
 * miscompile `safeToSpeculate` describes, because an offset past 2^32 is a negative one and a range
 * compared across it wraps. The walk the other way - from a root down through every use of its
 * address - is `addressContained`, and it is likewise the one containment question: the slot
 * collections in lower_promote.cpp and lower_split.cpp and `allocationEscapes` in lower_forward.cpp
 * ask it, and what their own walks keep is about *shape* - and, in lower_split.cpp, where the walk
 * stops (`collectUses` says why).
 */
LowerPlace placeOf(LowerBase base, LowerPtr<LowerValue> pointer, U64 bytes);

// The extent an access states, where it states one - `kUnbounded` for a copy or fill whose count is
// not a literal.
U64 placeExtent(LowerBase base, const LowerAccess& access);
U64 placeEnd(const LowerPlace& place);

// Whether two places may share a byte - see the definition.
bool mayOverlap(const LowerPlace& a, const LowerPlace& b);

// The allocations of a function whose address is used for nothing but their own accesses, keyed by
// the allocation's result - the `contained` table `placeOf` reads.
void collectContainedAllocations(LowerBase base, LowerFunction& fun, HashMap<U32, U8>& into);
