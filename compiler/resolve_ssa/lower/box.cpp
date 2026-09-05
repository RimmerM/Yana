#include "internal.h"

namespace ssa {

/*
 * `@box`, and the allocation an indirect field is - `doc/spec/repr.md`'s `@box`, and
 * `mir::RecordField::boxed`.
 *
 * The whole of the feature below the MIR is here. Above this file the indirection does not exist:
 * `fieldPlace` hands back the same owned projection an inline field gets, move paths are built over
 * it, drops are placed on it, and the loan checker sees a field of a record. That is the one thing
 * this stage does differently from the shipping one, which appends a `Deref` to the place path and
 * carries the boxed edge through every place walk in the compiler.
 *
 * What is left is three operations, and they are the three this file is:
 *
 *  - **making** one - `createBox`, at a construction, which allocates and fills the slot;
 *  - **reaching** through one - `boxTarget`, which is the load `fieldAddress` performs;
 *  - **releasing** one - `emitBoxTeardown`, the target's own obligations and then its storage.
 *
 * The last is where the interesting decision is, and `BoxRelease` in `internal.h` states it: whether
 * the storage is the program's to hand back is a question about *how the release site reached the
 * value*, and there is a site - an overwrite through a `&` borrow - where the answer is not visible.
 * `known.def`'s `isHeapAddress` describes the two ways a build answers it there.
 */

// The argument is in internal.h, beside the declaration.
LowerPtr<LowerFunction> Lowering::roleFunction(intrinsic::Known role, LocationId source) {
    auto declared = program.roles.function(role);
    if(!declared) {
        context.diagnostics.error(
            "this program needs the heap, and the standard library declares nothing with the role %@"_v,
            source, intrinsic::knownDef(role).role);
        return nullptr;
    }

    auto found = functions.getValue(declared.offset);
    if(!found) {
        /*
         * Reachable rather than impossible, and the failure is an *ordering* one: every function
         * this module emits has a signature before any body is lowered, so a role that is missing
         * here is a role whose claimant the reachability walk never marked. `markReachable` roots
         * the three allocator roles for exactly that reason.
         */
        context.diagnostics.error(
            "internal: %@ was reached before it was lowered"_v, source,
            intrinsic::knownDef(role).role);
        return nullptr;
    }

    return found.unwrap();
}

void Lowering::emitFreeHeap(LowerBlock& block, LowerPtr<LowerValue> address, LocationId source) {
    auto callee = roleFunction(intrinsic::Known::freeHeap, source);
    if(!callee) return;

    auto name = block.addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

    call(lower, to, block, 0, 2, lower[callee]->callType, [&](LowerInstCall* built) {
        built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
        built->used()[1] = address;
    });
}

/*
 * Where a boxed field's provenance bit lives, as a word address and a mask.
 *
 * Zero mask where this build stores none - the mode whose allocator answers `isHeapAddress` - and
 * where the field is not a box. A byte at a time rather than the whole flags word, so that a record
 * with more boxes than a word has bits needs no second shape: the bit index picks the byte and the
 * offset inside it, and both ends of the pair compute it the same way.
 */
struct BoxFlag {
    U32 offset = 0;
    U8 mask = 0;

    bool exists() const { return mask != 0; }
};

static BoxFlag boxFlagOf(Layout layout, DeclPtr<mir::Type> record, U16 field,
                         DeclPtr<mir::OwnershipSchema> shape) {
    auto repr = reprOf(layout, record, shape);
    if(!repr.boxFlagsBytes) return BoxFlag();

    auto held = repr.fields.get(layout.base, field);
    if(!held.hasBoxFlag()) return BoxFlag();

    return BoxFlag { repr.boxFlagsOffset + U32(held.boxFlagBit / 8), U8(1u << (held.boxFlagBit % 8)) };
}

LowerPtr<LowerValue> Lowering::boxTarget(LowerBlock& block, LowerPtr<LowerValue> slot) {
    auto loaded = block.addInst(lower, new (to.arena) LowerInstLoad(
        slot, StringId(), LowerType::Pointer, layout.target.pointerSize, false));

    return ((LowerInstSingle*)loaded)->created().ptr - lower;
}

// The argument is in internal.h, beside the declaration.
LowerPtr<LowerValue> Lowering::createBox(LowerBlock& block, LowerPtr<LowerValue> owner,
                                         DeclPtr<mir::Type> record, U16 field,
                                         DeclPtr<mir::OwnershipSchema> shape, LocationId source) {
    auto repr = reprOf(layout, record, shape);
    auto held = repr.fields.get(decl, field);
    auto slot = offsetAddress(block, owner, held.offset);

    auto callee = roleFunction(intrinsic::Known::allocateHeap, source);
    if(!callee) return nullptr;

    /*
     * The **target's** stride and not its size, which is the same number an array element takes and
     * is the right one for the same reason: the bytes handed out have to be enough for a whole value
     * of the type, trailing padding included, because nothing else is going to be placed in them.
     * A record's `size` stops at its last field so that a *container* can land inside its padding,
     * and a box has no container to do that.
     */
    auto manifest = manifestNative(layout, held.occurrence);
    auto bytes = strideOf(manifest);

    auto size = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), LowerType::Int64, U64(bytes)));

    auto name = block.addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

    auto allocated = call(lower, to, block, 1, 2, lower[callee]->callType,
                          [&](LowerInstCall* built) {
        new (built->created().ptr) LowerValue(built, LowerType::Pointer, StringId());
        built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
        built->used()[1] = ((LowerInstSingle*)size)->created().ptr - lower;
    });

    auto address = allocated->created().ptr - lower;

    /*
     * Zeroed where the target has a word nothing writes in full - `hasPaddedWord`, and the same
     * reason `frameStorage` zeroes a frame slot.
     *
     * `allocateHeap` hands back whatever was in the block, and a boxed value whose own record
     * carries a provenance word or a co-packed run would read that as its starting bits. This is
     * the second allocation site in the stage and therefore the second place the rule has to be
     * stated; there is no third.
     */
    if(hasPaddedWord(layout, held.occurrence.type)) {
        U32 at = 0;
        while(at < bytes) {
            auto width = bytes - at >= 8 ? 8u : bytes - at >= 4 ? 4u : bytes - at >= 2 ? 2u : 1u;

            block.addInst(lower, new (to.arena) LowerInstStore(
                offsetAddress(block, address, at), immediate(block, LowerType::Int64, 0), width));

            at += width;
        }
    }

    // The pointer into the field's own slot, which is what the field *is*.
    block.addInst(lower, new (to.arena) LowerInstStore(slot, address, layout.target.pointerSize));

    /*
     * And the provenance, where this build records it.
     *
     * Always **set**, because every box this stage makes comes from `allocateHeap`: region placement
     * is a rung nothing selects yet, and a box on the stack is the optimization the flag exists to
     * make possible rather than one anything performs. So today the bit is written to one at every
     * construction and read at every ambiguous release, which is a bit that costs a store and
     * answers correctly - and the day a placement decides otherwise, this is the one line that has
     * to learn about it.
     *
     * A read-modify-write of the byte rather than a store of the whole flags word: two boxed fields
     * of one record share a byte, and a construction that stored the word would clear its
     * neighbour's bit. The storage arrives zeroed, so the `or` alone is the whole of it.
     */
    auto flag = boxFlagOf(layout, record, field, shape);
    if(flag.exists()) {
        auto at = offsetAddress(block, owner, flag.offset);

        auto previous = block.addInst(lower, new (to.arena) LowerInstLoad(
            at, StringId(), LowerType::Int32, 1, false));

        auto bit = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), LowerType::Int32, U64(flag.mask)));

        auto merged = block.addInst(lower, new (to.arena) LowerInstBinary(
            StringId(), LowerType::Int32, ((LowerInstSingle*)previous)->created().ptr - lower,
            ((LowerInstSingle*)bit)->created().ptr - lower, LowerInst::Or));

        block.addInst(lower, new (to.arena) LowerInstStore(
            at, ((LowerInstSingle*)merged)->created().ptr - lower, 1));
    }

    return address;
}

/*
 * The teardown of one boxed field, as a function - see the declaration for why it is a call.
 *
 * Interned on four things, and each of them makes a different function:
 *
 *  - the **target type**, which decides what the obligations are;
 *  - the two **halves**, because an authored `Drop` on the enclosing record replaces the derived
 *    walk for its half alone and the member has to run exactly what is left - the same key
 *    `teardownHalves` carries, for the same reason;
 *  - the **release**, which is what this file is about. `Owned` frees unconditionally, `Foreign`
 *    frees nothing, and `Unknown` asks - of a flag the caller passes, or of the allocator.
 *
 * A `Unknown` release in the stored-bit mode takes a **second argument**: the bit lives in the
 * enclosing record and only the caller has that record in hand. In the classified mode it takes one
 * argument and asks `isHeapAddress` itself, which is what makes that mode's records narrower - the
 * question moved from the value to the address.
 */
LowerPtr<LowerFunction> Lowering::boxTeardownFor(DeclPtr<mir::Type> target, bool runDrop,
                                                 bool runReclaim, BoxRelease release,
                                                 LocationId source) {
    auto stored = storesBoxProvenance(layout);
    auto flagged = release == BoxRelease::Unknown && stored;

    auto key = mir::InternKey().mix(target.offset).mix(U64(runDrop)).mix(U64(runReclaim))
                               .mix(U64(release) + 8).hash;
    if(auto found = boxTeardowns.getValue(key)) return found.unwrap();

    auto ownership = mir::ownershipOf(decl, target);

    // Named by what it does to the storage, since the type alone would print the same line for
    // three functions that differ in exactly that.
    auto text = String("box(") + mir::typeText(context, decl, target);
    if(!runDrop && ownership.drop != mir::TeardownKind::None) text = text + String(" no drop");
    if(!runReclaim && ownership.reclaim != mir::TeardownKind::None) {
        text = text + String(" no reclaim");
    }

    switch(release) {
        case BoxRelease::Owned:   break;
        case BoxRelease::Unknown: text = text + String(stored ? " if owned" : " if heap"); break;
        case BoxRelease::Foreign: text = text + String(" keep"); break;
    }

    auto function = to.addFunction(erasedName(text + String(")")));
    auto argument = function->addArg(lower, StringId(), LowerType::Pointer);

    // The provenance the caller read out of the record, where this mode has one. Declared before
    // anything is emitted, because a signature has to be complete before the body that a recursive
    // type reaches back into it from.
    LowerArg* owns = flagged ? function->addArg(lower, StringId(), LowerType::Int32) : nullptr;

    auto handle = function - lower;

    // Recorded before the walk, which is what makes a recursive type terminate: `data List = Nil |
    // Cons {tail: List}` reaches its own teardown through this edge, and finds the function it is
    // in the middle of building.
    boxTeardowns.add(key, handle);

    auto block = function->addBlock(lower, StringId());
    auto address = &argument->result - lower;

    /*
     * The target's own obligations, through the pointer - the ordinary walk, which is why nothing
     * about a box appears in it.
     *
     * `release` travels **into** it rather than `Owned` being assumed, because a box inside a box is
     * an allocation of the same provenance: whatever placed the value placed all of it, so a value
     * this site may not hand back holds interior allocations it may not hand back either. Under
     * `Unknown` each of them asks its own question - of the bit in *this* target's record, or of the
     * allocator - and under `Foreign` none of them is freed at all.
     */
    emitTeardown(*block, address, target, runDrop, runReclaim, source, release);

    if(release == BoxRelease::Foreign) {
        emitReturn(*block, nullptr, 0);
        return handle;
    }

    if(release == BoxRelease::Owned) {
        emitFreeHeap(*block, address, source);
        emitReturn(*block, nullptr, 0);
        return handle;
    }

    /*
     * The conditional release, which is the case this whole arrangement exists for.
     *
     * The test differs between the two modes and nothing else does: the stored mode tests the bit
     * its caller passed, and the classified mode asks the allocator whether the address is one of
     * its own. Both answer a value that is zero or not, so one `je` serves them.
     */
    LowerPtr<LowerValue> condition = nullptr;

    if(flagged) {
        condition = &owns->result - lower;
    } else {
        auto callee = roleFunction(intrinsic::Known::isHeapAddress, source);
        if(!callee) return handle;

        auto name = block->addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

        auto answered = call(lower, to, *block, 1, 2, lower[callee]->callType,
                             [&](LowerInstCall* built) {
            new (built->created().ptr) LowerValue(built, LowerType::Int32, StringId());
            built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
            built->used()[1] = address;
        });

        condition = answered->created().ptr - lower;
    }

    auto free = function->addBlock(lower, StringId());
    auto done = function->addBlock(lower, StringId());

    block->addInst(lower, new (to.arena) LowerInstJe(condition, free - lower, done - lower));

    emitFreeHeap(*free, address, source);
    free->addInst(lower, new (to.arena) LowerInstJmp(done - lower));

    emitReturn(*done, nullptr, 0);
    return handle;
}

// The argument is in internal.h, beside the declaration.
void Lowering::emitBoxTeardown(LowerBlock& block, LowerPtr<LowerValue> owner,
                               DeclPtr<mir::Type> record, U16 field,
                               DeclPtr<mir::OwnershipSchema> shape, bool runDrop, bool runReclaim,
                               BoxRelease release, LocationId source) {
    auto repr = reprOf(layout, record, shape);
    auto held = repr.fields.get(decl, field);

    auto slot = offsetAddress(block, owner, held.offset);
    auto address = boxTarget(block, slot);

    /*
     * The provenance bit, read where the release needs one - and read **here** rather than inside
     * the callee, because the bit is a field of the enclosing record and the callee is handed only
     * the target's address. That is the whole asymmetry between the two modes: the stored one has to
     * carry the answer across the call, and the classified one recomputes it from the address.
     */
    auto flag = boxFlagOf(layout, record, field, shape);
    auto flagged = release == BoxRelease::Unknown && flag.exists();

    LowerPtr<LowerValue> owns = nullptr;

    if(flagged) {
        auto at = offsetAddress(block, owner, flag.offset);

        auto byte = block.addInst(lower, new (to.arena) LowerInstLoad(
            at, StringId(), LowerType::Int32, 1, false));

        auto mask = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), LowerType::Int32, U64(flag.mask)));

        auto bit = block.addInst(lower, new (to.arena) LowerInstBinary(
            StringId(), LowerType::Int32, ((LowerInstSingle*)byte)->created().ptr - lower,
            ((LowerInstSingle*)mask)->created().ptr - lower, LowerInst::And));

        owns = ((LowerInstSingle*)bit)->created().ptr - lower;
    }

    auto callee = boxTeardownFor(held.occurrence.type, runDrop, runReclaim, release, source);
    auto name = block.addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

    call(lower, to, block, 0, owns ? 3 : 2, lower[callee]->callType, [&](LowerInstCall* built) {
        built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
        built->used()[1] = address;
        if(owns) built->used()[2] = owns;
    });
}

} // namespace ssa
