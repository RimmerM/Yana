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
 *     scratch one at each instruction that touches it. Those are reserved for the whole function
 *     rather than found after the fact, so the function is placed again with them held back. This
 *     can only happen once.
 *   - a web would rather have taken a register from a cheaper occupant than gone without one. The
 *     occupant was placed earlier in the same walk, and everything after it has been decided
 *     against that, so it is left homeless from the start of the next pass instead.
 *
 * Neither can fail to converge: the reserved set is monotone and settles after one change, and the
 * forced-spill set only grows and is bounded twice over - by kMaxEvictions and by there being
 * finitely many webs. Whatever no longer fits in a register goes to the frame, and the frame has no
 * limit.
 *
 * The first pass is the answer for a function that fitted in its registers, which is most of them,
 * and which pays nothing for any of the above.
 */

// How many webs an allocation may displace across all of its passes. Every displacement costs one
// more placement pass, and the improvement from each is small and diminishing, so this bounds what a
// pathological function can spend. Reaching it costs code quality and nothing else: a pass is a
// complete, correct placement whether or not another one would have been better.
static constexpr Size kMaxEvictions = 16;

FunctionRegs allocateRegisters(Context& ctx, LowerBase base, LowerFunction& fun, const MachineFunction& machine) {
    auto& constraints = targetConstraints();
    auto& convention = constraints.getConvention(fun.callType);
    auto live = fun.buildLiveness(base);

    // Whether rbp is this function's frame pointer or one more register to hand out. Asked once,
    // here, and given to both the allocator and (through FunctionRegs) frame layout: the two
    // deciding it separately is the one way this can go wrong quietly, since a value placed in rbp
    // and a frame addressed through rbp are each individually correct.
    auto framePointer = functionNeedsFramePointer(ctx, base, fun);
    auto reserved = framePointer ? framePointerRegs() : RegSet {};

    Array<bool> forceSpill;
    for(Size i = 0; i < live->valueMap.size(); i++) forceSpill.push(false);

    Placement placement;
    bool scratchReserved = false;
    Size evictions = 0;

    for(;;) {
        placement = computePlacement(base, fun, *live, machine, constraints, reserved, forceSpill);
        bool again = false;

        if(placement.needsScratch && !scratchReserved) {
            reserved |= spillTempRegs();
            scratchReserved = true;
            again = true;
        }

        for(auto id: placement.evicted) {
            if(forceSpill[id] || evictions >= kMaxEvictions) continue;

            forceSpill[id] = true;
            evictions++;
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

    auto legalized = legalizeFunction(base, fun, machine, constraints, placement);

    FunctionRegs result;

    // Which of the registers the function writes its caller expects to get back untouched. Both
    // halves count: the ones placement handed to webs or found instructions clobbering, and the
    // scratch registers legalization used. The prologue saves exactly these and the epilogue
    // restores them; a function that never left its convention's clobber set saves nothing.
    result.usedCalleeSaved = (placement.writtenPhysical | legalized.writtenPhysical) & convention.calleeSaved;
    result.framePointer = framePointer;
    result.placement = ::move(placement);
    result.legalized = ::move(legalized);

    // The register model already describes banks whose moves, spills and encodings do not exist -
    // see the note on `reg` in gen.cpp. A location in one reaching emission would be written out as
    // an integer instruction with a vector register number in it, which is wrong in a way nothing
    // downstream can notice, so it is rejected here rather than left to a golden.
    assertTrue(result.usedCalleeSaved.banks[BankVector] == 0); // no encoder saves a vector register yet

    assertTrue(verifyAllocation(ctx, base, fun, *live, machine, constraints, result));
    return result;
}
