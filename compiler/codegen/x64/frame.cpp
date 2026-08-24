#include "gen.h"
#include "x64_util.h"

/*
 * Frame layout.
 *
 * The allocator says what the function needs stack space for; this decides where that space is. The
 * split matters because the two questions have different inputs: the allocator knows which values
 * could not stay in registers, and only after it has finished is it known how many callee-saved
 * registers were touched, whether rsp is going to move during the body, and what alignment the
 * calls in the function demand - all of which move every offset.
 *
 * Everything here is arithmetic on the picture in gen.h's FrameLayout comment. The one decision
 * with any judgement in it is whether to establish a frame pointer, which is what the rest of this
 * comment is about.
 *
 * That decision is not made here, though it used to be. It is made by functionNeedsFramePointer
 * below, which the allocator calls before it starts, because the answer decides whether rbp is a
 * register values can be put in - and the allocator and this pass disagreeing about that would put
 * a value in the register the frame is addressed through. Nothing about the decision needs
 * allocation to have run: it reads the frame-pointer mode, whether the function calls anything, and
 * whether it has a dynamic alloca, all of which are properties of the IR as it stands.
 *
 * rbp costs a push, a move, a pop and one of the fifteen allocatable registers for the whole
 * function, so `FramePointerMode::Needed` spends it only where an rsp-relative frame would be
 * wrong. That is now one case, and only one: a dynamic alloca moves rsp by an amount not known
 * until the function runs, so nothing after it can name a fixed frame object relative to rsp at all.
 *
 * Outgoing call arguments used to be a second case, because opening an area per call moved rsp
 * across the argument setup and any rsp-relative reference taken in the middle of it would be off
 * by however much had been opened so far. Reserving the area once in the prologue removed that:
 * rsp now stays where the prologue left it for the whole body.
 *
 * `NonLeaf` and `All` are the debugging positions: a leaf function's frame has nothing above it to
 * walk to, so `NonLeaf` gives up the register only where a stack walk could continue, and `All`
 * gives it up everywhere.
 */

static bool callsAnything(LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        for(auto i: base[offset]->instructions.contents(base)) {
            if(base[i]->kind == LowerInst::Call) return true;
        }
    }

    return false;
}

/*
 * An alloca whose size is not an immediate moves rsp at run time. Read straight from the IR rather
 * than from `FrameObjects::hasDynamicAlloca`, which is the same test made by collectFrameObjects and
 * is not available yet at the point this has to be answered.
 *
 * **This over-reports, and every caller is one where over-reporting is the safe direction.**
 * `isImm` wants `LowerValue::Implicit` as well as the kind - the flag saying the constant has been
 * folded into the instruction that reads it - and nothing sets that flag until the constant passes
 * run. So at any point before those passes this answers "dynamic" for a fixed-size alloca too.
 *
 * The one reader left is `functionNeedsFramePointer`, which is asked before allocation and where a
 * wrong "yes" costs a register in a function that turns out not to need one. A wrong *"no"* would be
 * unrecoverable - `register.cpp` asserts that a dynamic alloca has a frame pointer - so the test may
 * not be sharpened here to whatever the kind happens to be mid-pipeline: a pass is free to
 * materialize a constant into a register or fold one out of it, and this runs before both.
 *
 * The **refusal** that used to read it too has moved, for exactly that reason - see the note in
 * computeFrameLayout. A refusal answered conservatively rejects programs that compile.
 */
static bool hasDynamicAlloca(LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        for(auto i: base[offset]->instructions.contents(base)) {
            auto inst = base[i];
            if(inst->kind != LowerInst::Alloca) continue;
            if(!isImm(base[((LowerInstAlloca*)inst)->byteCount])) return true;
        }
    }

    return false;
}

// The strongest boundary anything in this function needs rsp to be on, read from the IR: what the
// conventions of the calls it makes require of the stack pointer at the call, and what the fixed
// objects it allocates require of their addresses.
//
// A dynamic allocation is not here. Its own alignment is satisfied by rounding the address it produces
// (see emitAlloca), which is local to the instruction and costs the rest of the frame nothing.
static U32 requiredStackAlignment(LowerBase base, LowerFunction& fun, const Constraints& constraints) {
    U32 required = 8;

    auto raise = [&](U32 alignment) {
        if(alignment > required) required = alignment;
    };

    for(auto offset: fun.blocks.contents(base)) {
        for(auto i: base[offset]->instructions.contents(base)) {
            auto inst = base[i];

            if(inst->kind == LowerInst::Call) {
                auto type = ((LowerInstCall*)inst)->getCallType();
                raise(constraints.getConvention(type).stackAlignment);
            }

            // Fixed only, which is the same test collectFrameObjects makes when it decides whether
            // the allocation becomes a frame object at all.
            if(inst->kind == LowerInst::Alloca) {
                auto alloca = (LowerInstAlloca*)inst;
                if(isImm(base[alloca->byteCount])) raise(alloca->alignment);
            }
        }
    }

    return required;
}

/*
 * And the strongest boundary a *spill slot* could ask for, which is the one part of the answer the
 * IR does not state outright.
 *
 * The allocator decides what spills and it has not run yet, so which slots exist is not knowable
 * here. What is knowable is how wide each of them could be: a spill slot's class is
 * `stackSlotClassFor` of the value's type and nothing else (see takeSlot), so the *set* of
 * alignments a function's slots can be drawn from is a property of its value types alone. Reading
 * the set rather than the membership is what lets the question be asked this early at all.
 *
 * It answers 8 for every function the lowering produces today, since no scalar spills wider than a
 * word. A packed vector value is the first thing that raises it, and it is the reason this exists:
 * a slot wanting more alignment than the entry convention promises is a realignment, and a
 * realignment needs a frame pointer, and whether rbp is a frame pointer has to be settled before
 * the allocator starts.
 */
static U32 spillStackAlignment(LowerBase base, LowerFunction& fun) {
    U32 required = 8;

    auto raise = [&](LowerType type) {
        auto alignment = stackSlotSize(stackSlotClassFor(type));
        if(alignment > required) required = alignment;
    };

    // Every instruction, arguments included: a `LowerArg` is an instruction of the entry block and
    // the value it creates is spilled by the same rule as any other.
    for(auto offset: fun.blocks.contents(base)) {
        for(auto i: base[offset]->instructions.contents(base)) {
            auto inst = base[i];
            for(auto& value: inst->created()) raise(value.type);
        }
    }

    return required;
}

/*
 * Whether the prologue may have to establish a stronger boundary than it was entered on.
 *
 * A *may* rather than a *does*, and the difference is the spill slots: a function holding a value
 * that would need an over-aligned slot pays for one only if the allocator actually spills it, which
 * is not known until it has. The exact answer is taken in computeFrameLayout below, once the slots
 * exist; what this answers is the question that has to be settled before allocation, which is
 * whether rbp has to be held back as a frame pointer in case the answer turns out to be yes.
 *
 * Being conservative here costs a register in a function that turns out not to need it. Being
 * conservative about the *realignment* would cost a mask, a second base and every local's addressing
 * mode, which is why the two are separated rather than answered once.
 */
bool functionMayRealignStack(LowerBase base, LowerFunction& fun, const Constraints& constraints) {
    // A function is entered with rsp on whatever boundary its own convention promises, and padding
    // can only preserve that - so anything stronger has to be established here.
    auto entry = constraints.getConvention(fun.callType).stackAlignment;
    return requiredStackAlignment(base, fun, constraints) > entry || spillStackAlignment(base, fun) > entry;
}

/*
 * The one frame this backend cannot build, reported where a program can still be stopped.
 *
 * A realigning prologue puts the locals below a mask and addresses them through rsp; a dynamic
 * alloca moves rsp out from under them. Keeping both would take a third base register held for the
 * whole function, which nothing here reserves - so the combination is refused.
 *
 * Refused *unconditionally*, which is the whole reason this exists as a function. It used to be an
 * `assertTrue` inside computeFrameLayout, and assertTrue compiles away in a release build: the same
 * program that stopped in a debug build emitted a frame whose locals move under it in a release one,
 * which is the worst of the two possible answers.
 */
static void reportUnsupportedFrame(Context& ctx, LowerFunction& fun) {
    ctx.diagnostics.error("x64: %@ both allocates at run time and needs a stack alignment stronger than its calling convention promises, and this backend cannot build a frame that does both - the alignment moves the locals below the stack pointer and the allocation moves the stack pointer out from under them"_v,
                          nullptr, ctx.findName(fun.name));
}

/*
 * What the *mode* asks for, with nothing about this function's frame in it.
 *
 * Split out of `functionNeedsFramePointer` because the layout asks it again. The two questions
 * that function answers are not the same question: "does the frame need one" is settled exactly,
 * after allocation, and "may the frame need one" has to be settled before it - so the half that
 * does not change between the two is the one worth stating once.
 */
static bool modeNeedsFramePointer(Context& ctx, LowerBase base, LowerFunction& fun) {
    switch(ctx.settings.framePointer) {
        case FramePointerMode::All:
            return true;
        case FramePointerMode::NonLeaf:
            return callsAnything(base, fun);
        case FramePointerMode::Needed:
            return false;
    }

    return false;
}

bool functionNeedsFramePointer(Context& ctx, LowerBase base, LowerFunction& fun) {
    // Neither of these is a preference. After a dynamic alloca there is no fixed relationship between
    // rsp and anything; after a realignment the distance from rsp back to the frame is only known at
    // run time. Both override every mode below.
    if(hasDynamicAlloca(base, fun)) return true;
    if(functionMayRealignStack(base, fun, targetConstraints())) return true;

    return modeNeedsFramePointer(ctx, base, fun);
}

static U32 alignUp(U32 value, U32 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

FrameLayout computeFrameLayout(Context& ctx, LowerBase base, LowerFunction& fun, const Constraints& constraints, const FunctionRegs& regs) {
    auto& frame = regs.placement.frame;

    FrameLayout layout;

    // Split by bank, because the two are preserved by quite different means: a general register is
    // pushed, and a vector one has no push at all and takes a region of the frame instead. Nothing
    // else in this layout has to know which is which - the push accounting reads `savedRegs` and the
    // region reads `savedVectors`.
    regs.usedCalleeSaved.iterate([&](PhysicalReg saved) {
        assertTrue(saved.bank == BankGpr || saved.bank == BankVector); // a bank with no way to save it
        (saved.bank == BankGpr ? layout.savedRegs : layout.savedVectors).add(saved);
    });

    // Taken from the allocation rather than decided here: the allocator was given the same answer
    // before it ran, and handed rbp out as an ordinary register if it was false.
    layout.framePointer = regs.framePointer;
    layout.base = layout.framePointer ? framePointerReg() : stackPointerReg();

    assertTrue(!frame.hasDynamicAlloca || layout.framePointer); // rsp moves, so rsp cannot be the base
    assertTrue(!layout.framePointer || !layout.savedRegs.has(framePointerReg())); // saved twice otherwise

    auto savedCount = U32(layout.savedRegs.count());

    // Locals first, then spill slots grouped widest-first so that each group lands on its own
    // alignment without padding between the slots inside it. Offsets here are measured upwards from
    // the bottom of the *local* region, and turned into displacements from the base register once
    // everything below that region is known.
    Array<U32> offsetInRegion;
    for(Size i = 0; i < frame.slots.size(); i++) offsetInRegion.push(0);

    U32 size = 0;
    U32 maxAlign = 8;

    auto place = [&](StackSlotKind kind, StackSlotClass cls, bool byClass) {
        for(Size i = 0; i < frame.slots.size(); i++) {
            auto& slot = frame.slots[i];
            if(slot.kind != kind) continue;
            if(byClass && slot.slotClass != cls) continue;

            size = alignUp(size, slot.alignment);
            offsetInRegion[i] = size;
            size += slot.size;

            if(slot.alignment > maxAlign) maxAlign = slot.alignment;
        }
    };

    place(StackSlotKind::Local, StackSlotClass::Slot32, false);

    for(Size c = kStackSlotClassCount; c > 0; c--) {
        place(StackSlotKind::Spill, StackSlotClass(c - 1), true);
    }

    // The callee-saved vector registers go above the slots, in one region of their own. It is not
    // aligned to the width it holds and does not ask the frame to be, and that is now a choice
    // rather than an impossibility: raising `maxAlign` here would work - the layout below reads it
    // like any other - but it would buy nothing. The unaligned encoding is correct at any alignment
    // and exactly as long, and Complex is entered on eight, so asking for sixteen here would turn
    // every function that saves a vector register into a realigning one. See kClassMoves in gen.cpp.
    auto vectorSaveRegion = size;
    size += vectorSaveSize() * U32(layout.savedVectors.count());

    // The boundary rsp has to be on for the whole body: whatever the calls in this function expect to
    // find at the call, and whatever the objects in its frame need of their addresses. The second is
    // not a property of rsp on its own - a local is an offset from a base, and an offset on its
    // boundary only lands on that boundary if the base does too.
    auto alignment = frame.callAlignment > maxAlign ? frame.callAlignment : maxAlign;

    /*
     * Whether the prologue has to establish that boundary itself - decided here, from the slots the
     * allocator actually made, rather than from the pre-pass's guess about them.
     *
     * The two are separate questions and they are answered at different times on purpose. Whether
     * rbp is a frame pointer has to be settled *before* allocation, because the allocator either
     * hands the register out or does not; whether the prologue realigns has to be settled *after*,
     * because a spill slot wider than a word is the first thing that can demand it and no one knows
     * what spilled until it has run. functionMayRealignStack answers the first conservatively, which
     * is what makes the answer here free to be exact.
     *
     * So the assertion below is the seam between them: a realignment the pre-pass did not see coming
     * is a frame with no register to address itself through.
     */
    auto entryAlignment = constraints.getConvention(fun.callType).stackAlignment;
    layout.realignsStack = alignment > entryAlignment;

    assertTrue(!layout.realignsStack || layout.framePointer); // realigning loses the distance to rsp

    /*
     * Not supported together: a realigning frame keeps its locals below the mask and addresses them
     * through rsp, and a run-time allocation moves rsp out from under them. Supporting both would
     * take a third base register held for the whole function, which nothing reserves.
     *
     * **Reported here and nowhere else, which is a change.** It used to be asked twice: once at the
     * top of `transformFunction`, over the IR as it arrived, and again here for the one demand that
     * form could not see - a realignment a *spill slot* asked for, since nothing knew what would
     * spill until allocation had run.
     *
     * The early copy is gone because neither of the two facts it needed is settled that early.
     * `frame.hasDynamicAlloca` is `collectFrameObjects`' answer and is exact; the same question
     * asked over the arriving IR is not, because `isImm` wants a flag the constant passes have not
     * set yet, so every fixed-size alloca read as dynamic. Being wrong in that direction is fine for
     * a frame pointer and not fine for a refusal - it rejects programs that compile, which is what
     * the first `@convention(sysv)` callee ran into: it raised its callers' required alignment above
     * what their own convention promised, and all three were refused on the strength of a 16-byte
     * alloca of a slice descriptor. Sharpening the early test instead would have traded that for the
     * unrecoverable direction, since a pass may materialize a constant into a register or fold one
     * out of it and that test runs before both.
     *
     * So the question is asked once, at the only point where both halves of it are exact. Nothing is
     * emitted for this function until the layout it returns is built.
     *
     * What is left after the report is only to build *a* frame rather than an inconsistent one, and
     * the frame that is still internally consistent is the one that does not realign. It is
     * under-aligned for whatever asked for the alignment, which is what the diagnostic says; nothing
     * runs it.
     */
    if(layout.realignsStack && frame.hasDynamicAlloca) {
        reportUnsupportedFrame(ctx, fun);
        layout.realignsStack = false;
    }

    /*
     * And the other side of that seam: a frame pointer the pre-pass reserved for a realignment that
     * did not happen.
     *
     * `functionMayRealignStack` answers for every function that *holds* a value wider than a word,
     * because a spill of one would demand a boundary the entry convention does not promise. Every
     * function with a vector in it is that function, and most of them never spill - so every leaf
     * in the vector library was carrying `push rbp ; mov rsp,rbp ; leave` for a slot that was never
     * made. Three instructions and four bytes each, on functions whose whole body is a loop.
     *
     * The conservatism has to stay where it is: rbp is either handed to the allocator or held back,
     * and that cannot be revisited once the allocator has run. What can be revisited is whether the
     * *prologue* establishes one, which is this - and by here `realignsStack` is exact, so the
     * question has a real answer. rbp is simply not used by the function that gives it up; it was
     * never in the pool, so nothing is in it and nothing has to move.
     *
     * The mode is asked again rather than inherited. `-frame-pointer all` and `nonleaf` are requests
     * for a frame pointer whatever the frame needs, and a debugger walking rbp does not care that
     * this function turned out to spill nothing.
     */
    if(layout.framePointer && !layout.realignsStack && !frame.hasDynamicAlloca &&
       !modeNeedsFramePointer(ctx, base, fun))
    {
        layout.framePointer = false;
        layout.base = stackPointerReg();
    }

    // Locals and spill slots hang off rsp whenever there is no frame pointer, and also whenever the
    // prologue realigns - that is where the aligned region is. Otherwise they sit directly below the
    // saved registers, at a fixed distance from rbp.
    auto rspRelativeLocals = layout.realignsStack || !layout.framePointer;

    // The outgoing argument area is the lowest thing in the frame, so the local region starts above
    // it - far enough above that the area's size cannot push a local off its own alignment. In a
    // realigning frame the whole region below the mask is measured from an rsp the mask aligned, so
    // both the area and the padding above it are whole numbers of boundaries.
    auto localBase = alignUp(frame.argAreaSize, layout.realignsStack ? alignment : maxAlign);

    U32 padding;

    if(layout.realignsStack) {
        // The mask leaves rsp on `alignment`; everything the prologue then reserves has to be a whole
        // number of those, or rsp is off the boundary again by the time a call reads it.
        padding = alignUp(size, alignment) - size;
    } else {
        // Everything the prologue moves rsp by, including the return address the call pushed, has to
        // add up to a multiple of the alignment - that is what turns "rsp was aligned when we were
        // called" into "rsp is aligned when we call".
        auto prologueBytes = 8 + (layout.framePointer ? 8 : 0) + 8 * savedCount + localBase + size;
        padding = alignUp(prologueBytes, alignment) - prologueBytes;
    }

    // Padding goes at the top of the local region rather than the bottom of the frame, so that the
    // argument area stays exactly at rsp where a callee expects to find it.
    auto localArea = size + padding;

    layout.fixedSize = localBase + localArea;
    layout.argAreaSize = frame.argAreaSize;
    layout.dynamicAlignment = alignment;

    auto localsBase = rspRelativeLocals ? stackPointerReg() : framePointerReg();
    auto regionBase = rspRelativeLocals ? I32(localBase) : -I32(8 * savedCount + localArea);

    layout.vectorSaveBase = localsBase;
    layout.vectorSaveOffset = regionBase + I32(vectorSaveRegion);

    // An incoming argument sits in the caller's frame, above the return address the call pushed.
    // With a frame pointer that is a fixed distance from rbp; without one it is measured from where
    // rsp came to rest, so it moves with everything the prologue did.
    auto incomingBase = layout.framePointer ? I32(16) : I32(8 + 8 * savedCount + layout.fixedSize);

    for(Size i = 0; i < frame.slots.size(); i++) {
        auto& slot = frame.slots[i];

        if(slot.kind == StackSlotKind::IncomingArg) {
            layout.slotBase.push(layout.base);
            layout.slotOffset.push(incomingBase + I32(slot.argOffset));
        } else {
            layout.slotBase.push(localsBase);
            layout.slotOffset.push(regionBase + I32(offsetInRegion[i]));
        }
    }

    return layout;
}
