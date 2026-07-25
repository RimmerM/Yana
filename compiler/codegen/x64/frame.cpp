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

bool functionNeedsFramePointer(Context& ctx, LowerBase base, LowerFunction& fun) {
    // Not a preference: after a dynamic alloca there is no fixed relationship between rsp and
    // anything, so this overrides every mode below.
    if(hasDynamicAlloca(base, fun)) return true;

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
    auto& frame = regs.frame;

    FrameLayout layout;
    layout.savedRegs = regs.usedCalleeSaved;

    // Taken from the allocation rather than decided here: the allocator was given the same answer
    // before it ran, and handed rbp out as an ordinary register if it was false.
    layout.framePointer = regs.framePointer;
    layout.base = layout.framePointer ? framePointerReg() : stackPointerReg();

    assertTrue(!frame.hasDynamicAlloca || layout.framePointer); // rsp moves, so rsp cannot be the base
    assertTrue(!layout.framePointer || !layout.savedRegs.has(framePointerReg())); // saved twice otherwise

    U32 savedCount = 0;
    layout.savedRegs.iterate([&](PhysicalReg) { savedCount++; });

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

    // Everything the prologue moves rsp by, including the return address the call pushed, has to
    // add up to a multiple of the alignment the calls in this function expect to find - that is
    // what turns "rsp was aligned when we were called" into "rsp is aligned when we call".
    //
    // Which means the entry alignment has to be at least as good as anything the body asks for.
    // A function whose own convention promises less than one of its callees demands would have to
    // realign rsp at run time and keep a second base register to find its frame afterwards, which
    // is not implemented: the two described conventions are 8 and 16, so this is the case of a
    // Complex function calling a SysV one, and the fix is to declare it SysV as well.
    auto entryAlignment = constraints.getConvention(fun.callType).stackAlignment;
    assertTrue(frame.callAlignment <= entryAlignment); // caller cannot guarantee the alignment a callee needs

    auto alignment = frame.callAlignment > maxAlign ? frame.callAlignment : maxAlign;

    // The outgoing argument area is the lowest thing in the frame, so the local region starts above
    // it - far enough above that the area's size cannot push a local off its own alignment.
    auto localBase = alignUp(frame.argAreaSize, maxAlign);

    auto prologueBytes = 8 + (layout.framePointer ? 8 : 0) + 8 * savedCount + localBase + size;
    auto padding = alignUp(prologueBytes, alignment) - prologueBytes;

    // Padding goes at the top of the local region rather than the bottom of the frame, so that the
    // argument area stays exactly at rsp where a callee expects to find it.
    layout.fixedSize = localBase + size + padding;
    layout.argAreaSize = frame.argAreaSize;
    layout.dynamicAlignment = alignment;

    // The fixed region sits directly below the saved registers, so its bottom - which is where rsp
    // ends up - is that far below the base. Locals start `localBase` above that.
    auto regionBase = (layout.framePointer ? -I32(8 * savedCount + layout.fixedSize) : 0) + I32(localBase);

    // An incoming argument sits in the caller's frame, above the return address the call pushed.
    // With a frame pointer that is a fixed distance from rbp; without one it is measured from where
    // rsp came to rest, so it moves with everything the prologue did.
    auto incomingBase = layout.framePointer ? I32(16) : I32(8 + 8 * savedCount + layout.fixedSize);

    for(Size i = 0; i < frame.slots.size(); i++) {
        auto& slot = frame.slots[i];

        if(slot.kind == StackSlotKind::IncomingArg) {
            layout.slotOffset.push(incomingBase + I32(slot.argOffset));
        } else {
            layout.slotOffset.push(regionBase + I32(offsetInRegion[i]));
        }
    }

    return layout;
}
