#pragma once

#include "lower_inst.h"

/*
 * What a loop recomputes because it was written inside it - §10 item 1 and §50.2 of
 * test/bench/findings.md.
 *
 * Two kinds of thing move, and they are the same rule asked of two different obligations.
 *
 * A read whose address does not change between iterations and whose answer nothing in the loop can
 * change is one read, done once. `hashOf` in test/bench/programs/Text.yana is the shape: the string's
 * base pointer is a field of the parameter, so every iteration of the scan loads it again before
 * indexing off it - `mov (%rdi),%r8` as one of the eight instructions in the corpus's worst `-Os`
 * row.
 *
 * A *computation* whose operands do not change between iterations is one computation, done once.
 * `Matrix.multiply` in test/bench/programs/Matrix.yana is that shape: `out[row * n + column]` and
 * `b[k * n + column]` recompute `row * n` and `k * n` in the innermost loop, where neither `row`, `k`
 * nor `n` changes. §50.2 hand-hoisted both in the source and measured **152.5 -> 150.7 ms, 1.2%** -
 * the one remaining difference between this backend's innermost loop and `-Os`'s that is not a
 * decision already measured and kept.
 *
 * ## Why here and not above the fork
 *
 * `hoistLoopValues` in `compiler/opt` - the optimizer of the stage deleted in J173 - was the same
 * idea over the resolve IR, and it could not reach either. The read is `text.bytes.run.items`,
 * whose place is rooted in a `borrow` of the parameter - and the borrow is *re-taken inside the
 * loop*, because that is what the resolver emitted for each call that read through it. So the
 * place's root was a value defined in the loop, the hoister's `operandsOutside` refused it before
 * any aliasing question was asked, and hoisting the borrow itself is a decision that tier declines
 * to take (Analysis-Optimization.md §7): a loan moved out of a loop covers iterations the borrow
 * check never agreed to.
 *
 * `row * n` is not in the resolve IR as a multiplication at all on every target. It is `row * n` on a
 * native one, and on JS the same subscript is a place with an Index projection - the same split
 * lower_induction.h §1 gives for why the induction reduction is a lower pass. What is common to the
 * two shapes is arithmetic over lower values, and that is what exists down here.
 *
 * Down here the borrow has become an address, the address is `%text` itself, and the question is the
 * plain one - is this pointer the same every time round, and does anything in the loop write.
 *
 * ## What "does not change between iterations" is
 *
 * An operand defined outside the loop, which for a value in SSA form is simply where its instruction
 * sits. That is also, and without a second check, a value that is available in the preheader: the
 * preheader's only successor is the header, so anything it does not itself dominate is reached
 * through the header and cannot dominate the header in turn. A definition outside the loop that
 * reaches a use inside it therefore dominates the preheader, and an instruction moved there still has
 * every operand it had.
 *
 * An `Imm` is the exception and is admitted wherever it sits, because a constant has no operands and
 * moving it is free of every question above. It is *moved with* its reader rather than swept out
 * ahead of it: an immediate is usually folded into the encoding of whatever reads it, so relocating
 * one nothing hoisted would be a `mov` in a preheader in exchange for nothing.
 *
 * ## The two things that have to be true, for a load
 *
 * **Nothing in the loop writes the bytes it reads.** Asked once per loop as a list of the places the
 * loop writes (`LoopWrites` in lower_licm.cpp, J158), and each candidate is compared against it with
 * `forwardStoredValues`' own aliasing rule (`mayOverlap` in lower_store.h): two allocations are two
 * objects, a contained allocation is reached by nothing but its own address, and one root is compared
 * by byte range. A write naming no address - a call, an intrinsic - may write anything but a
 * contained allocation, and anything ordered or volatile in the loop ends the question. Until J158
 * this was "nothing in the loop writes at all", which refused every loop that stored anything.
 *
 * What that still cannot tell apart is the commonest pair: an element store through a pointer
 * loaded out of a record, against a read of that record's header - `xs[j + 1] = xs[j]` in `Sort`'s
 * insertion loop beside `xs.length`. Two roots neither of which is an allocation may be anything,
 * and the fact that would separate them - that an exclusive borrow's referent is reached through no
 * other pointer - lives a stage above, in the ownership types this IR does not carry.
 *
 * **The read cannot fault where it is being moved to.** The preheader runs when the loop is entered
 * and the body runs only if the test passes, so a load moved from one to the other happens on a path
 * it did not happen on before - and a `while i < n` with `n` zero is that path. Two answers are
 * accepted, and both are about the *address* rather than about the loop:
 *
 *  - it is an `alloca` and the whole access is inside it, which is storage the frame reserved;
 *  - the same base is already read or written, at an offset reaching at least as far, by an
 *    instruction that dominates the preheader. `hashOf` is this one: the entry block loads the
 *    string's length at offset 12, so the object is at least 16 bytes and the field at offset 0 is
 *    inside it.
 *
 * A dynamic offset is admitted by neither, which is what keeps every element access out of this: an
 * indexed address is not loop-invariant in the first place, and a checked one is only in bounds
 * behind the check.
 *
 * ## And the three that have to be true for a computation
 *
 * **It has to be a computation that can be moved.** `isRepeatable` is the question, and this pass takes
 * its answer whole - it is `kLowerRepeatable`, a column of inst.def. That is a change: the four dividing operations used to come out of it here, because the
 * machine raises on a zero divisor and on `INT_MIN / -1`, and a division a loop performs behind a
 * guard on its divisor is a division the preheader would perform in front of one. They go back in
 * because the fault is gone rather than because the argument was wrong - `makeDivisionTotal` in
 * lower_divide.cpp has already replaced every division with one that answers both of those divisors
 * instead of raising on them, which is most of what the language defining them buys. Nothing carrying that bit
 * can fault now: a shift count is masked, a float operation answers a NaN, a vector operation
 * is lane-wise arithmetic, and a division answers 0.
 *
 * One exception survives, and it is a narrow one: a division `makeDivisionTotal` left *unguarded*
 * because a test above it had already settled the divisor. That one cannot fault where it stands and
 * would above the test, so `mayFault` reads the bit it carries and this pass leaves it alone. It is
 * the only computation here whose safety is a property of its position rather than of its operands,
 * and it is worth knowing that anything else faulting given that bit would need the same
 * treatment - while the other two readers of it, which ask at a point the instruction already
 * runs at, would need none.
 *
 * **It has to be a computation the loop actually performs.** An address, a comparison and a copy are
 * each free where they stand - a displacement in an addressing mode, a condition code, a coalesced
 * register - and hoisting one buys a live range and sells nothing. See `worthHoisting`, which is the
 * difference between this pass costing `Sort` 5.2 ms and costing it nothing.
 *
 * **And the loop it leaves has to be an innermost one.** Which is the profitability question, and the
 * answer to it is a rule rather than a model: what a hoist buys is one instruction per iteration of
 * the loop it leaves, and what it costs is a value live across every iteration of every loop *inside*
 * that one. Leaving an innermost loop is where the second term is empty and the first is largest.
 * `Matrix.multiply`'s two invariant multiplies are the demonstration of both halves - see
 * `isInnermost`. A value that ends up invariant one level further out still gets there, on the next
 * round, once the loop it landed in is itself the innermost one.
 *
 * ## Where it runs
 *
 * After `eliminateCommonValues`, so that a loop reading one address twice is shown one load rather
 * than two, and before `reduceInductionVariables`, which is where the induction pass expects the
 * loop's invariant parts to have stopped moving. That order matters more for the computations than
 * for the loads: the induction pass reads a stride as a literal and an address as `base + (i << k)`,
 * and an invariant multiply still sitting in the loop body is one more thing between the two.
 */
void hoistLoopInvariants(LowerBase base, LowerModule& module, LowerFunction& fun,
                         const LoopAnalysis& analysis);

/*
 * A location a loop both reads and writes, carried in a register across it - J158, the scalar
 * promotion of a loop-carried memory cell.
 *
 * `hoistLoopInvariants` takes a read whose bytes the loop does not write out of the loop. A read of
 * bytes the loop *does* write is the other half: the value it answers is available - it is whatever
 * the last store in the loop wrote, or what the cell held when the loop was entered - so the read is
 * redundant on the back edge and only partially redundant at the header, where the entry edge has
 * not got it. The rewrite is the classic one: the preheader reads the cell once, the header merges
 * that with what the latch ends with, and every read in the loop becomes the value the cell holds
 * above it.
 *
 * **The stores stay.** A promotion that also sinks the stores to the exits writes on a path that did
 * not write before - the exit a loop leaves by before its first store - and for storage anything
 * outside this function can reach that is a store another thread may observe. Keeping them costs a
 * store per iteration and needs no argument at all: memory is exactly what it was at every point,
 * and a read outside the loop, or a call inside it that reads, still sees the cell. What goes is the
 * reads, which are the latency; a store retires into the store buffer.
 *
 * What has to hold, for one cell:
 *
 *  - its address is a root defined outside the loop plus a constant offset (`placeOf`), and every
 *    read of it in the loop reads the same width into the same type with the same extension;
 *  - every store to it writes the read's type at that width - the value a read returns is then the
 *    stored value itself, with nothing to rebuild (`storedAnswer`);
 *  - **nothing else in the loop may write a byte of it**, which is `LoopWrites` with its own stores
 *    excepted: another store under the same root at an overlapping range, a copy or a fill, a store
 *    through a root that is not provably another object, a call - unless the cell is in a contained
 *    allocation, which no call was handed - and anything ordered or volatile, which ends the loop;
 *  - the preheader's read cannot fault: `safeToSpeculate`, the hoister's rule;
 *  - the loop has a preheader and one latch, and every block of it but the header is entered from
 *    inside it.
 *
 * Innermost loops first, so a cell promoted in an inner loop leaves one read in that loop's
 * preheader, which the enclosing loop's promotion then answers in turn.
 *
 * **Capped by its phis** (J160): a cell read once in a state machine's loop needs a phi at every
 * join the machine has, so `CellRewrite::plan` counts the phis that survive the collapse, the
 * header's own excepted, and declines where they outnumber the loads they replace. `test/base` and
 * the bench binaries are byte-identical either way; the iterator goldens are what it shrinks.
 *
 * In front of the hoister, which then finds the reads this left - the preheader's own among them -
 * and takes what the enclosing loop does not write further out.
 */
//
// Answers two things. `sank`: whether it moved the stores of a contained cell out of a loop, which
// leaves that slot with no access inside the loop at all - the shape `promoteStackSlots` takes whole,
// and the caller runs it again for exactly that reason. `promoted`: whether it carried any cell,
// which leaves a read in a preheader that may stand behind a store of the same word - J180. The
// lexer's `parseVariable` stores `self.p` and enters its identifier loop, and the read this placed
// was a round trip through memory the forwarding had already run past; the caller forwards again.
struct CellPromotion {
    bool promoted = false;
    bool sank = false;
};

CellPromotion promoteLoopCells(LowerBase base, LowerModule& module, LowerFunction& fun, const LoopAnalysis& analysis);
