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
 * rbp costs a push, a move, a pop and one of the fourteen allocatable registers for the whole
 * function, so `FramePointerMode::Needed` spends it only where an rsp-relative frame would be
 * wrong. That is two cases, and only two:
 *
 *  - A dynamic alloca moves rsp by an amount not known until the function runs, so nothing after it
 *    can name a fixed frame object relative to rsp at all.
 *  - A call that passes arguments by pushing them moves rsp across the argument setup, so an
 *    rsp-relative reference taken in the middle of that sequence - reading a spilled value into an
 *    argument register, say - would be off by however much had been pushed so far. This only
 *    matters if there is something to reference: a function with no frame objects can push all it
 *    likes.
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

static bool needsFramePointer(Context& ctx, LowerBase base, LowerFunction& fun, const FrameObjects& frame) {
    // Not a preference: after a dynamic alloca there is no fixed relationship between rsp and
    // anything, so this overrides every mode below.
    if(frame.hasDynamicAlloca) return true;

    switch(ctx.settings.framePointer) {
        case FramePointerMode::All:
            return true;
        case FramePointerMode::NonLeaf:
            return callsAnything(base, fun);
        case FramePointerMode::Needed:
            return frame.hasPushedCallArgs && !frame.isEmpty();
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
    layout.framePointer = needsFramePointer(ctx, base, fun, frame);
    layout.base = makeRegId(GenReg, layout.framePointer ? U16(IntRegister::rbp) : U16(IntRegister::rsp));

    U32 savedCount = 0;
    for(Size i = 0; i < kRegCount; i++) {
        if(layout.savedRegs & (U64(1) << i)) savedCount++;
    }

    // Locals first, then spill slots grouped widest-first so that each group lands on its own
    // alignment without padding between the slots inside it. Offsets here are measured upwards from
    // the bottom of the fixed region, and turned into displacements from the base register once the
    // region's total size is known.
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

    auto prologueBytes = 8 + (layout.framePointer ? 8 : 0) + 8 * savedCount + size;
    auto alignment = frame.callAlignment > maxAlign ? frame.callAlignment : maxAlign;
    size += alignUp(prologueBytes, alignment) - prologueBytes;

    layout.fixedSize = size;
    layout.dynamicAlignment = alignment;

    // The fixed region sits directly below the saved registers, so its bottom - which is where rsp
    // ends up - is that far below the base.
    auto regionBase = layout.framePointer ? -I32(8 * savedCount + size) : 0;

    // An incoming argument sits in the caller's frame, above the return address the call pushed.
    // With a frame pointer that is a fixed distance from rbp; without one it is measured from where
    // rsp came to rest, so it moves with everything the prologue did.
    auto incomingBase = layout.framePointer ? I32(16) : I32(8 + 8 * savedCount + size);

    for(Size i = 0; i < frame.slots.size(); i++) {
        auto& slot = frame.slots[i];

        if(slot.kind == StackSlotKind::IncomingArg) {
            layout.slotOffset.push(incomingBase + I32(8 * slot.ordinal));
        } else {
            layout.slotOffset.push(regionBase + I32(offsetInRegion[i]));
        }
    }

    return layout;
}
