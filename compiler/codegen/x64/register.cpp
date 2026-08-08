#include "gen.h"
#include "x64_util.h"

/*
 * Register allocation.
 *
 * Two passes, and the whole point of this file is that they are two:
 *
 *   computePlacement (place.cpp)     where every web lives, over the whole function at once.
 *   legalizeFunction (legalize.cpp)  what each instruction does with that.
 *
 * Placement is free to think again about a web it has already placed, because nothing downstream
 * exists yet - no instruction names a location until legalization puts one there. So the two things
 * a placement can discover that it cannot act on itself are answered by placing the function again,
 * which costs one pass over the webs rather than a complete allocation and emission:
 *
 *   - a web ended up without a register, and one that has no register has to be brought into a
 *     scratch one at each instruction that cannot read it where it is. Those are reserved for the
 *     whole function rather than found after the fact, so how many of them legalizing this placement
 *     would take is measured - by legalizing it and recording what it asked for - and the function is
 *     placed again with that much held back.
 *   - a web would rather have taken a register from a cheaper occupant than gone without one. The
 *     occupant was placed earlier in the same walk, and everything after it has been decided
 *     against that, so it is left homeless from the start of the next pass instead.
 *
 * Neither can fail to converge: the reserve only ever grows and is bounded by the register file, and
 * the forced-homeless set only grows and is bounded twice over - by kMaxDisplacements and by there
 * being finitely many webs. Whatever no longer fits in a register goes to the frame, and the frame
 * has no limit.
 *
 * The first pass is the answer for a function that fitted in its registers, which is most of them,
 * and which pays nothing for any of the above.
 */

// How many webs an allocation may displace across all of its passes. Every displacement costs one
// more placement pass, and the improvement from each is small and diminishing, so this bounds what a
// pathological function can spend. Reaching it costs code quality and nothing else: a pass is a
// complete, correct placement whether or not another one would have been better.
static constexpr Size kMaxDisplacements = 16;

/*
 * The record arena - see RecordArena in gen.h.
 */

void* RecordArena::alloc(Size bytes) {
    // Rounded so that whatever lands next is aligned as well as this did. Everything committed here
    // is a run of a fixed-size record, and the word is the widest field any of them has.
    bytes = (bytes + 7) & ~Size(7);

    // Past the end of the chunk being filled, so move on to the next. A chunk an earlier function
    // grew is taken over whole rather than freed, so only a module wider than any before it makes
    // this list longer.
    if(chunk >= chunks.size() || used + bytes > sizes[chunk]) {
        if(chunk < chunks.size()) chunk++;
        used = 0;

        // A request no chunk would hold gets one of its own. That cannot happen for a record, but
        // it is what keeps the rule from being "the caller has to know the chunk size".
        auto want = bytes > kChunkBytes ? bytes : kChunkBytes;

        if(chunk == chunks.size()) {
            chunks.push((Byte*)Tritium::hAlloc(want));
            sizes.push(want);
        } else if(sizes[chunk] < bytes) {
            Tritium::hFree(chunks[chunk]);
            chunks[chunk] = (Byte*)Tritium::hAlloc(want);
            sizes[chunk] = want;
        }
    }

    auto out = chunks[chunk] + used;
    used += bytes;
    return out;
}

RecordArena::~RecordArena() {
    for(auto chunk: chunks) Tritium::hFree(chunk);
}

void allocateRegisters(Context& ctx, LowerBase base, LowerFunction& fun, const MachineFunction& machine,
    RegScratch& scratch, FunctionRegs& result)
{
    auto& constraints = targetConstraints();
    auto& convention = constraints.getConvention(fun.callType);
    auto live = fun.buildLiveness(base);

    // Emptied rather than replaced, here and in the two passes below: what the previous function
    // left is the storage this one is about to ask for, and the allocation of a module is a few
    // thousand of these buffers if each function builds its own.
    result.clear();

    // How often each block runs relative to the entry, which is what every cost the placement weighs
    // is stated in. Computed once here rather than inside each pass: it is a function of the CFG and
    // the edge metadata, and nothing on the loop below touches either.
    // Kept in the result rather than local: emission weighs a jump against the same numbers (§7.2),
    // and the move-assignment reuses whatever buffer the previous function grew.
    result.frequency = fun.buildFrequencies(base);
    auto& frequency = result.frequency;

    // Whether rbp is this function's frame pointer or one more register to hand out. Asked once,
    // here, and given to both the allocator and (through FunctionRegs) frame layout: the two
    // deciding it separately is the one way this can go wrong quietly, since a value placed in rbp
    // and a frame addressed through rbp are each individually correct.
    auto framePointer = functionNeedsFramePointer(ctx, base, fun);

    // One row per value, emptied rather than reallocated - the same rule every other buffer here
    // follows. A row is the set of registers earlier passes over *this* function took away from that
    // web; a fresh function starts with every row empty.
    auto& displacedFrom = scratch.displacedFrom;
    while(displacedFrom.size() < live->valueMap.size()) displacedFrom.push(RegSet {});
    for(Size i = 0; i < displacedFrom.size(); i++) displacedFrom[i] = RegSet {};

    // The placement is written into the result rather than assigned to it, so a second pass over
    // this function - and the next function after it - reuses everything the first one grew. Nothing
    // reads it between passes: each one restates the whole of it.
    auto& placement = result.placement;
    Size displacements = 0;

    // Nothing held back to begin with, which is the answer for a function that fitted in its
    // registers. The reserve below only ever grows, which is half of what makes this loop terminate.
    TemporaryReserve temporaries;

    for(;;) {
        computePlacement(base, fun, *live, machine, constraints, frequency, framePointer,
            temporaries, displacedFrom, scratch, placement);
        bool again = false;

        // A web with no register has to be brought into a scratch one at the instructions that cannot
        // read it where it is. How many that takes, and in which banks, is not a property of the
        // function but of this placement of it - so it is measured against this placement rather than
        // guessed at, and the measurement is the pass that will spend the answer.
        //
        // The reserve is raised rather than replaced: holding more back changes where the next pass
        // puts things, so the next measurement is a different question and not a correction of this
        // one. Taking the larger of the two can over-reserve slightly and can never under-reserve.
        if(placement.requiresLegalizationTemps) {
            auto demand = measureTemporaryReserve(base, fun, machine, constraints, placement, scratch);
            if(temporaries.growTo(demand)) again = true;
        }

        // A repeat is dropped rather than counted: the register is already out of that web's reach,
        // so granting it again would spend a displacement and change nothing. That is also what
        // keeps the loop finite - a register only ever enters one of these sets.
        for(auto& request: placement.displacementRequests) {
            if(displacements >= kMaxDisplacements) break;
            if(displacedFrom[Size(request.web)].has(request.reg)) continue;

            displacedFrom[Size(request.web)].add(request.reg);
            displacements++;
            again = true;
        }

        if(!again) break;
    }

    // The decision was made before placement started, so this is only a check that it was made from
    // the same facts the placement then produced.
    assertTrue(!placement.frame.hasDynamicAlloca || framePointer);

    // Debug builds only - assertTrue compiles away entirely in a release build, taking the call with
    // it. Placement is checked on its own terms before anything is resolved against it, so a wrong
    // location is reported against the web that got it rather than against the instruction that
    // eventually read it.
    assertTrue(verifyPlacement(ctx, base, fun, *live, machine, constraints, placement, framePointer));

    legalizeFunction(base, fun, machine, constraints, placement, temporaries, scratch, result.legalized);

    // Which of the registers the function writes its caller expects to get back untouched. Both
    // halves count: the ones placement handed to webs or found instructions clobbering, and the
    // scratch registers legalization used. The prologue saves exactly these and the epilogue
    // restores them; a function that never left its convention's clobber set saves nothing.
    result.usedCalleeSaved =
        (placement.writtenPhysical | result.legalized.writtenPhysical) & convention.calleeSaved;
    result.framePointer = framePointer;
    result.temporaries = temporaries;

    // The register model still describes one bank whose moves and encodings do not exist - every
    // `kmov` is VEX-encoded, and see the note on `reg` in gen.cpp. A location in it reaching
    // emission would be written out with a register number no legacy encoding can name, which is
    // wrong in a way nothing downstream can notice, so it is rejected here rather than left to a
    // golden.
    assertTrue(result.usedCalleeSaved.banks[BankMask] == 0); // no encoder preserves a mask register

    assertTrue(verifyAllocation(ctx, base, fun, *live, machine, constraints, result));
}
