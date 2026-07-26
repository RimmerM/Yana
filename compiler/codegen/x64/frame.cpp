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

// An alloca whose size is not an immediate moves rsp at run time. Read straight from the IR rather
// than from FrameObjects::hasDynamicAlloca, which is the same test made by collectFrameObjects and
// is not available yet at the point this has to be answered.
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

bool functionRealignsStack(LowerBase base, LowerFunction& fun, const Constraints& constraints) {
    // A function is entered with rsp on whatever boundary its own convention promises, and padding
    // can only preserve that - so anything stronger has to be established here.
    return requiredStackAlignment(base, fun, constraints)
        > constraints.getConvention(fun.callType).stackAlignment;
}

bool functionNeedsFramePointer(Context& ctx, LowerBase base, LowerFunction& fun) {
    // Neither of these is a preference. After a dynamic alloca there is no fixed relationship between
    // rsp and anything; after a realignment the distance from rsp back to the frame is only known at
    // run time. Both override every mode below.
    if(hasDynamicAlloca(base, fun)) return true;
    if(functionRealignsStack(base, fun, targetConstraints())) return true;

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
    // aligned to the width it holds and does not ask the frame to be: raising the frame's alignment
    // here is not possible - the decision was taken before the allocator ran and before anything
    // knew a vector register would be saved - so the saves use the unaligned encoding instead, which
    // is correct at any alignment and exactly as long. See kClassMoves in gen.cpp.
    auto vectorSaveRegion = size;
    size += kVectorSaveSize * U32(layout.savedVectors.count());

    // The boundary rsp has to be on for the whole body: whatever the calls in this function expect to
    // find at the call, and whatever the objects in its frame need of their addresses. The second is
    // not a property of rsp on its own - a local is an offset from a base, and an offset on its
    // boundary only lands on that boundary if the base does too.
    auto alignment = frame.callAlignment > maxAlign ? frame.callAlignment : maxAlign;

    // Whether the prologue has to establish that boundary itself. Answered before allocation ran,
    // because realigning needs a frame pointer and the allocator had to be told; what is checked here
    // is only that the answer covered everything the frame turned out to need. A spill slot wanting
    // more than 8 is what would not have been visible from the IR - and cannot happen while every
    // value the lowering produces is a scalar, since a scalar spills at its own width and no scalar
    // is wider than eight bytes. A packed vector value would be the first to break that, which is
    // why the saved vector registers above take unaligned stores rather than raising `maxAlign`.
    auto entryAlignment = constraints.getConvention(fun.callType).stackAlignment;
    layout.realignsStack = functionRealignsStack(base, fun, constraints);

    assertTrue(layout.realignsStack || alignment <= entryAlignment); // an alignment the pre-pass did not see
    assertTrue(!layout.realignsStack || layout.framePointer);        // realigning loses the distance to rsp

    // Not supported together: a realigning frame keeps its locals below the mask and addresses them
    // through rsp, and a run-time allocation moves rsp out from under them. Supporting both would take
    // a third base register held for the whole function, which nothing reserves.
    assertTrue(!layout.realignsStack || !frame.hasDynamicAlloca);

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
