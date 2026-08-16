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
 * None can fail to converge. The reserve's growth is bounded by the register file and its shrinking
 * by kMaxReserveShrinks; the forced-homeless set only grows and is bounded twice over - by
 * kMaxDisplacements and by there being finitely many webs. Whatever no longer fits in a register goes
 * to the frame, and the frame has no limit.
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
 * How many times an allocation may lower its scratch reserve.
 *
 * The reserve is measured against a placement and then held back from the *next* one, so the two
 * questions are not the same question: a pass that had every register spilled things a pass with two
 * held back does not, and asks for scratch the later pass does not need. Left at the high-water mark,
 * those registers are r15 downwards - callee-saved, so the function pushes and pops them for nothing.
 *
 * Lowering it is therefore worth a pass, and it is bounded here rather than by the arithmetic because
 * it can oscillate: fewer registers held back can mean more spilling, which asks for more scratch,
 * which holds more back. Two attempts is what settles the shapes this compiler produces, and hitting
 * the bound costs code quality and nothing else - every pass is a complete, correct placement, and
 * the loop only ever *ends* on one whose demand the reserve covers.
 */
static constexpr Size kMaxReserveShrinks = 2;

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

/*
 * §42 Which registers this function's scratch pool is drawn from.
 *
 * The pool used to be the top of the register file - r15 downwards, and xmm15 downwards - which was
 * one decision standing in for two, and had both of them backwards.
 *
 * **Safety.** The old argument was that r11-r15 are outside every described convention's argument
 * and result registers, so a scratch there can never collide with a fixed register the same
 * instruction is also placing. That is true of r12-r15 and false of r11, which `kComplexArgs` and
 * `kComplexResults` both name - and a reserve of five reaches it. The rule that is actually wanted
 * is about *this function*: a register no instruction of it fixes cannot collide with a fixed
 * operand, whatever the convention says in general. That is the first filter here, and it is
 * strictly stronger than the one it replaces.
 *
 * A **call** is where that filter earns its keep and where over-reading it would be fatal: its
 * argument and result registers are fixed operands and belong in the set, and its *clobber* set is
 * the callee's entire caller-saved half and does not - see the note on `note` below.
 *
 * **Cost.** r15 downwards is callee-saved, so every scratch register is a `push` and a `pop` in the
 * prologue and epilogue - and a leaf function that spills one value pays them while nine registers
 * its convention lets it destroy sit unused. Several resolve fixtures were paying exactly that for a
 * single short-lived constant. So among the registers that pass the filter, the ones the convention
 * clobbers come first.
 *
 * **What is *not* weighed is register pressure**, and that is deliberate. A pool register is one
 * register denied to placement for the whole function, and which one it is does not change how many
 * are left - so a crowded function spills the same amount whichever end the pool is taken from, and
 * only the prologue's cost differs. The allocation order (`buildOrder`) already spends registers
 * clobbered-first, which is the same preference read from the other side.
 *
 * Within each group the registers that need no REX prefix come first, which is one byte off every
 * instruction that names the scratch. That looks like it should collide with the filter - the low
 * registers are exactly the ones a convention fixes - and it does not, because a register the
 * function fixes is not in the group at all: what is left there is a low register the function
 * genuinely does not use, and that one is free *and* short.
 *
 * A function that fixes so much that the filter leaves too few registers falls back to what is left,
 * in the same descending order - which is no worse than what this replaced, since that took those
 * registers unconditionally.
 */
static void chooseTemporaryPool(LowerBase base, LowerFunction& fun, const MachineFunction& machine,
    const Constraints& constraints, bool framePointer, TemporaryReserve& out)
{
    // Every register this function's own instructions place a value in or write behind one's back.
    // Read from the shapes rather than from a placement: what an instruction fixes is a property of
    // the instruction, so this is the same answer before and after any pass.
    RegSet fixed;

    // One shape for the whole walk, which `shapeOf` empties before it fills - the same arrangement
    // every other walk over the shapes uses, and the difference between two allocations per
    // instruction and two per function.
    InstShape shape;

    /*
     * The **fixed operands** and not the clobbers, which is the distinction `formClobberRegs` in
     * legalize.cpp already draws and the one that decides how wide this filter is.
     *
     * A fixed operand is a register a parallel copy puts a value into at that instruction, so a
     * scratch there would overwrite an operand the instruction is about to read - and since the pool
     * is one choice for the whole function, a register fixed *anywhere* has to be out of it.
     *
     * A clobber is not that. It is a register the instruction's own expansion writes behind its
     * operands' backs, and `takeTemp` already steps over any pool position holding one - per
     * instruction, which is where the hazard is. Reading them here as well would be a filter that
     * empties itself: a *call*'s clobber set is the callee's whole caller-saved half, so every
     * function containing one would be left choosing between the two registers placement most needs.
     */
    auto note = [&](LowerInst* inst) {
        shapeOf(base, machine, constraints, fun, inst, shape);
        fixed |= fixedRegisters(shape);
    };

    for(auto a: fun.args.contents(base)) note((LowerInst*)base[a]);

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        for(auto i: block->instructions.contents(base)) note(base[i]);
        if(block->terminator) note(base[block->terminator]);
    }

    // The frame pointer is not a register this function has to hand out, and a scratch there would be
    // the frame addressed through a register the legalizer is about to overwrite.
    if(framePointer) fixed.add(framePointerReg());

    auto& convention = constraints.getConvention(fun.callType);
    auto& registers = targetRegisters();

    for(Size bank = 0; bank < kRegisterBankCount; bank++) {
        auto id = RegisterBankId(bank);
        auto& allocatable = registers.bank(id).allocatable;
        Size count = 0;

        /*
         * `low` is the encoding tie-break inside each group: a general register below r8 needs no REX
         * prefix, so a scratch there is one byte shorter at every instruction that names it. The
         * registers that would be cheapest to encode are also the ones a convention fixes - rax to a
         * result, rcx to a shift, rdi and rsi to arguments - so this only ever fires where the
         * function genuinely does not use them, and the filter above is what says so.
         *
         * Descending within each half, which keeps the choice stable: the low half is walked from
         * rdi down and the high from r15 down, so a function that frees one more register does not
         * reshuffle the ones it had.
         */
        auto pass = [&](bool clobbered, bool unfixed, bool low) {
            for(Size i = registers.bank(id).physicalCount; i-- > 0 && count < kMaxTemporaryPool;) {
                auto reg = PhysicalReg { id, U16(i) };

                if(!allocatable.has(reg)) continue;
                if((i < 8) != low) continue;
                if(convention.clobber.has(reg) != clobbered) continue;
                if(fixed.has(reg) == unfixed) continue;

                out.pool[bank][count++] = U8(i);
            }
        };

        // Clobbered and untouched - free, and the answer for most functions.
        pass(true, true, true);
        pass(true, true, false);

        // Preserved and untouched - a push and a pop, which is what this replaces.
        pass(false, true, true);
        pass(false, true, false);

        // And the fallback, for a function that fixes nearly everything.
        pass(true, false, true);
        pass(true, false, false);
        pass(false, false, true);
        pass(false, false, false);

        // A bank with nothing allocatable in it - the mask registers, which no form of this backend
        // selects - leaves the pool short, and a position read out of a short pool would name
        // register zero. Nothing asks for one, since nothing is ever placed in such a bank; the fill
        // is what makes that a fact about the table rather than a fact about who asks.
        auto available = Size(registers.bank(id).physicalCount);

        for(Size i = count; i < kMaxTemporaryPool; i++) {
            // Distinct by construction: the passes above take every allocatable register of the bank
            // before this runs, so a position left over means none of them is - and the count down
            // from the top then meets nothing that is already here.
            out.pool[bank][i] = U8(i < available ? available - 1 - i : 0);
        }
    }

    out.chosen = true;
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
    // Which loops there are, for the one decision that is stated over a *set of blocks* rather than
    // over a stretch of the numbering: promoting a homeless web into a register for a loop (§5.10).
    // Built here for the same reason the frequencies are - it is a function of the CFG, and nothing
    // on the loop below touches that - and handed to them, since deriving them is what the loops are
    // the expensive half of.
    auto loops = fun.buildLoops(base);

    result.frequency = fun.buildFrequencies(base, loops);
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
    // registers - but *which* registers would be held back is decided here, once, before any of them
    // is. Once, because the loop's own termination rests on the counts alone moving: a pool that
    // changed between passes would be a second thing for the two directions to chase.
    TemporaryReserve temporaries;
    chooseTemporaryPool(base, fun, machine, constraints, framePointer, temporaries);
    Size shrinks = 0;

    for(;;) {
        computePlacement(base, fun, *live, machine, constraints, frequency, loops, framePointer,
            temporaries, displacedFrom, scratch, placement);
        bool again = false;

        // A web with no register has to be brought into a scratch one at the instructions that cannot
        // read it where it is. How many that takes, and in which banks, is not a property of the
        // function but of this placement of it - so it is measured against this placement rather than
        // guessed at, and the measurement is the pass that will spend the answer.
        //
        // Raising it first, and only ever lowering it once nothing wants raising: holding more back
        // changes where the next pass puts things, so each measurement is a fresh question rather
        // than a correction of the last one, and the loop must not chase both directions at once.
        // Whichever way it last moved, it ends on a pass whose demand the reserve covers.
        //
        // A placement that could not need a scratch register at all answers zero without being
        // measured - `requiresLegalizationTemps` is what makes the common case cost nothing - and
        // that is the shape the lowering matters most for: a reserve measured against a crowded pass
        // and left standing over one that spills nothing is two callee-saved registers pushed and
        // popped for a temporary no instruction asks for. See kMaxReserveShrinks.
        TemporaryReserve demand;
        if(placement.requiresLegalizationTemps) {
            demand = measureTemporaryReserve(base, fun, *live, machine, constraints, placement, temporaries, scratch);
        }

        if(temporaries.growTo(demand)) {
            again = true;
        } else if(shrinks < kMaxReserveShrinks && temporaries.shrinkTo(demand)) {
            shrinks++;
            again = true;
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

    legalizeFunction(base, fun, *live, machine, constraints, placement, temporaries, scratch, result.legalized);

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
