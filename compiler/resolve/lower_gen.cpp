/*
 * The erased ABI: everything a generic body reads out of its environment instead of off a type.
 *
 * An unspecialized function does not know what its type variables are, so a size, an alignment, a
 * witness or a field's offset is a load from the environment its caller passed rather than a
 * constant. The concrete path is unchanged and sits beside each of these - see the `genContext`
 * test at the top of every one of them, which is what keeps the two forms comparable.
 */

#include "lower_internal.h"

// Defined below, beside the only other user of it.
static LowerPtr<LowerValue> tableAddress(LowerContext& lower, LowerBlock& block, ModulePtr<Global> table);

/*
 * The image anchor's address, which every table slot is measured from - see repr/table.h.
 *
 * One `lea r, [rip + image$base]` and no memory traffic: what is wanted is the symbol's *address*,
 * not anything stored at it, and nothing is stored at it. Pure and loop-invariant, so a body reading
 * several slots computes it once however many times this is called.
 */
static LowerPtr<LowerValue> imageBase(LowerContext& lower, LowerBlock& block) {
    return tableAddress(lower, block, lower.from.imageAnchor);
}

/*
 * The address one slot of a compiler-built table holds.
 *
 * Every erased read goes through here - an environment slot, a witness's method, a superclass
 * pointer, a property accessor, a descriptor's lifecycle half. One function because they are one
 * question, and because the encoding is not something six call sites should each know.
 *
 * A slot is four bytes holding `target - &anchor`, so this is a load, a sign-extension and an add
 * onto the anchor. See repr/table.h for why anchor-relative rather than absolute or self-relative -
 * the short version is that a GenEnv may have been built on the frame, and only a shared base
 * reaches the image from there.
 *
 * Signed, because a slot may name something in front of the anchor as easily as behind it.
 */
LowerPtr<LowerValue> tableSlotAddress(LowerContext& lower, LowerBlock& block,
                                      LowerPtr<LowerValue> table, U16 slot) {
    auto site = addOffset(lower, block, table, tableSlotOffset(slot));

    auto loaded = load(lower.lower, lower.to, block, lower.lower[site], 4, true, LowerType::Int32, StringId());
    auto widened = cast<true, true>(lower.lower, lower.to, block,
                                    lower.lower[loaded->created().ptr - lower.lower],
                                    LowerType::Int64, StringId());

    auto address = binary<LowerInst::Add>(lower.lower, lower.to, block,
                                          lower.lower[widened->created().ptr - lower.lower],
                                          lower.lower[imageBase(lower, block)],
                                          LowerType::Pointer, StringId());
    return address->created().ptr - lower.lower;
}

// The inverse: what goes *into* a slot for an address this frame computed. Only genEnvironment needs
// it, since every other table is a constant whose slots the assembler writes.
static LowerPtr<LowerValue> tableSlotValue(LowerContext& lower, LowerBlock& block,
                                           LowerPtr<LowerValue> address) {
    auto offset = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[address],
                                         lower.lower[imageBase(lower, block)],
                                         LowerType::Int64, StringId());
    return offset->created().ptr - lower.lower;
}

/*
 * Reading the environment.
 *
 * Slot N is at a fixed offset, so this is one load and one add. That is the whole of
 * Implementation-Generics.md part 1's "no runtime name lookup": no hashing, no search, no
 * comparison - the schema decided the number at compile time and the code loads it.
 */
static LowerPtr<LowerValue> genSlot(LowerContext& lower, LowerBlock& block, U16 slot) {
    return tableSlotAddress(lower, block, lower.genEnv, GenEnvFields::slot(slot));
}

/*
 * The witness one slot of the environment leads to.
 *
 * Usually the one in the slot, and one load further along for each superclass the path steps
 * through: a body holding `Num(a)` reaches `FromInt(a)` through the pointer the `Num` witness holds
 * for it rather than through a slot of its own. Every step is a byte offset the resolver worked out
 * from the class declarations - see genWitnessPath - so this is arithmetic and loads with nothing to
 * search, which is the property every other environment read has too.
 */
static LowerPtr<LowerValue> genWitness(LowerContext& lower, LowerBlock& block, U16 slot,
                                       ModuleList<U32, false> path) {
    auto witness = genSlot(lower, block, slot);

    for(auto step: path.contents(lower.local)) {
        witness = tableSlotAddress(lower, block, witness, U16(step));
    }

    return witness;
}

// The descriptor of a type this body cannot see, or null for one it can. A concrete type inside a
// generic body needs no descriptor: its size is a constant here exactly as it is anywhere else.
LowerPtr<LowerValue> genTypeDesc(LowerContext& lower, LowerBlock& block, TypePtr type) {
    if(!lower.genEnv || !type || !isGeneric(lower.global, type)) return nullptr;

    auto slot = genTypeSlot(*lower.genModule, *lower.genContext, type);

    // A generic type with no slot is one the schema never recorded, which would mean the body needs
    // something its own context does not promise. requireTypeSlot is what keeps that from happening;
    // reaching it here is a compiler bug rather than a program error.
    assertTrue(slot != maxLimit<U16>);
    return genSlot(lower, block, slot);
}

/*
 * A const parameter's value - Implementation-Const-Generics.md §3.2.
 *
 * One cell of the environment, and no step past it. A type variable's slot holds an *address* that a
 * metric then indexes into; a const one holds the number, so `descField` straight onto the
 * environment is the whole read - one load and one widen rather than three loads and an add.
 *
 * Null for a count this body already knows, which is the same shape genTypeDesc has and reads as the
 * same test: "is this one of the things my caller had to tell me".
 */
LowerPtr<LowerValue> genConstValue(LowerContext& lower, LowerBlock& block, TypePtr count, LowerType type) {
    if(!lower.genEnv || !count || !isGeneric(lower.global, count)) return nullptr;

    auto slot = genConstSlot(*lower.genModule, *lower.genContext, count);

    // A const variable with no slot would mean a body reading a count its own context does not
    // declare, which §2.5 makes unreachable: a count is a bare variable, and every variable of a
    // context is numbered from the declaration.
    assertTrue(slot != maxLimit<U16>);

    auto address = addOffset(lower, block, lower.genEnv, tableSlotOffset(GenEnvFields::slot(slot)));
    auto loaded = load(lower.lower, lower.to, block, lower.lower[address], kTableCellSize, false,
                       LowerType::Int32, StringId());
    auto value = loaded->created().ptr - lower.lower;

    /*
     * At the metric's own width and not at a machine word, which is the difference between this and
     * `descField` beside it - a size is always computed at 64 bits, and a count is a value of the
     * const parameter's *declared* type. A `Count` returning an Int64 where the signature says `Int`
     * is a lower-IR type error, and the validator says so.
     *
     * Unsigned in both directions: a count is a non-negative number, and the cell it came out of is
     * four bytes of one.
     */
    if(type == LowerType::Int32) return value;
    return cast<false, false>(lower.lower, lower.to, block, lower.lower[value], type,
                              StringId())->created().ptr - lower.lower;
}

// One U32 field of a descriptor, widened to the 64-bit form every size and offset is computed in.
LowerPtr<LowerValue> descField(LowerContext& lower, LowerBlock& block,
                                      LowerPtr<LowerValue> descriptor, U16 slot) {
    auto address = addOffset(lower, block, descriptor, tableSlotOffset(slot));
    auto loaded = load(lower.lower, lower.to, block, lower.lower[address], 4, false, LowerType::Int32, StringId());
    auto widened = cast<false, false>(lower.lower, lower.to, block,
                                      lower.lower[loaded->created().ptr - lower.lower],
                                      LowerType::Int64, StringId());

    return widened->created().ptr - lower.lower;
}

// A descriptor's alignment, which shares the flags cell rather than having one of its own - see
// TypeDescFields::kFlags. The load is the same load; the shift is what the packing costs, and it is
// paid only by an explicit `alignof` on a type variable.
LowerPtr<LowerValue> descAlign(LowerContext& lower, LowerBlock& block,
                               LowerPtr<LowerValue> descriptor) {
    auto word = descField(lower, block, descriptor, TypeDescFields::kFlags);
    auto shifted = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[word],
                                          lower.lower[immediate(lower, kPackedMetricShift)],
                                          LowerType::Int64, StringId());

    return shifted->created().ptr - lower.lower;
}

// How many bytes one value of this type occupies - a constant where the type is known, and a load
// out of its descriptor where it is not.
LowerPtr<LowerValue> sizeOfType(LowerContext& lower, LowerBlock& block, TypePtr type) {
    if(auto descriptor = genTypeDesc(lower, block, type)) {
        return descField(lower, block, descriptor, TypeDescFields::kSize);
    }

    return immediate(lower, typeSize(lower, type));
}

/*
 * Storage for one object, never of no size.
 *
 * A type whose fields all occupy nothing occupies nothing - a record of one unit field, which is
 * what a closure over a unit-typed binding captures into. Its *address* is still taken, though, and
 * a frame object of no size is not an address: the x64 placer has nothing to give one, and two of
 * them would be the same pointer. So a zero-size allocation is one word, which nothing ever reads.
 *
 * Only where the size is a constant. A size read out of a type descriptor may be zero at run time
 * for the same reason, and rounding it up there would be a branch on every allocation to buy the
 * same nothing - the erased paths reaching it allocate a word of their own instead.
 */
LowerPtr<LowerValue> storageSize(LowerContext& lower, LowerBlock& block, TypePtr type) {
    if(auto descriptor = genTypeDesc(lower, block, type)) {
        return descField(lower, block, descriptor, TypeDescFields::kSize);
    }

    return immediate(lower, max(typeSize(lower, type), 1u));
}

/*
 * What one slot of a run costs - the same question storageSize asks, in strides.
 *
 * Never rounded up to a word. A run of a zero-size element is a run of nothing, and that is the
 * right answer rather than the degenerate one storageSize avoids: the run's *address* still exists,
 * because the heap and the frame both hand back a real pointer for a request of no bytes, and
 * nothing indexes into slots there is no way to tell apart. Rounding here would multiply the
 * padding by the count instead.
 */
LowerPtr<LowerValue> strideSize(LowerContext& lower, LowerBlock& block, TypePtr type) {
    if(auto descriptor = genTypeDesc(lower, block, type)) {
        return descField(lower, block, descriptor, TypeDescFields::kStride);
    }

    return immediate(lower, typeStride(lower, type));
}

// `count * stride`, folded where both are immediates - which is every run whose length the literal
// wrote down. The multiply survives only for a run whose size the program computes.
LowerPtr<LowerValue> scaleBy(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> stride,
                                    LowerPtr<LowerValue> count) {
    auto isImmediate = [&](LowerPtr<LowerValue> value) {
        return lower.lower[value]->inst()->kind == LowerInst::Imm;
    };

    // Never nothing, for the reason storageSize is never nothing: a run of no slots still has an
    // address, and a frame object of no size is not one. `[] :: [Int]` is the everyday way to ask
    // for one, and the word it gets is never indexed into.
    if(isImmediate(stride) && isImmediate(count)) {
        return immediate(lower, max(((LowerImm*)lower.lower[count]->inst())->i *
                                    ((LowerImm*)lower.lower[stride]->inst())->i, U64(1)));
    }

    auto product = binary<LowerInst::Mul>(lower.lower, lower.to, block, lower.lower[stride],
                                          lower.lower[count], LowerType::Int64, StringId());
    return product->created().ptr - lower.lower;
}

// The address of one compiler-built constant table.
static LowerPtr<LowerValue> tableAddress(LowerContext& lower, LowerBlock& block, ModulePtr<Global> table) {
    auto name = lower.local[table]->name;
    auto target = lower.to.globals.getValue(name).unwrap();
    auto value = block.addInst(lower.lower, new (lower.to.arena) LowerInstGlobal(name, target));

    return value->created().ptr - lower.lower;
}

/*
 * The environment one generic call hands its callee - Implementation-Generics.md part 9.
 *
 * Three of its four cases land here. A call whose every slot was concrete has one interned constant
 * and nothing to build; a call from inside a generic body assembles a small table on the frame from
 * the addresses it knows and the slots it was handed itself. The fourth case, a specialized call,
 * never reaches lowering at all - it stopped being a generic call when it was specialized.
 *
 * The table is written once and never read back by this frame, so nothing about it needs to outlive
 * the call: an alloca is exactly the right storage, and the callee holding a pointer to it for the
 * duration of the call is the same contract an argument passed by address already has.
 */
LowerPtr<LowerValue> genEnvironment(LowerContext& lower, LowerBlock& block, InstGenCall& call) {
    if(call.env) return tableAddress(lower, block, call.env);

    auto& target = lower.repr.target;
    auto slots = call.fill;
    auto bytes = immediate(lower, tableSize(GenEnvFields::countFor(slots.size())));
    auto storage = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
        StringId(), bytes, target.pointerAlign));

    auto base = storage->created().ptr - lower.lower;
    U16 index = 0;

    for(auto slot: slots.contents(lower.local)) {
        /*
         * A count cell holds the number and not an address, so it is neither anchor-relative on the
         * way in nor decoded on the way out - Implementation-Const-Generics.md §3.1. A forwarded one
         * is the caller's own cell copied across unchanged, which is what the raw `descField` read
         * of the environment gives.
         */
        if(slot.count) {
            auto number = slot.isForwarded()
                ? descField(lower, block, lower.genEnv, GenEnvFields::slot(slot.forwarded))
                : immediate(lower, slot.value);

            auto cell = addOffset(lower, block, base, tableSlotOffset(GenEnvFields::slot(index)));
            block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(cell, number, kTableCellSize));
            index++;
            continue;
        }

        auto value = slot.isForwarded()
            ? genWitness(lower, block, slot.forwarded, slot.forwardedSupers)
            : tableAddress(lower, block, slot.constant);

        auto address = addOffset(lower, block, base,
                                 tableSlotOffset(GenEnvFields::slot(index)));

        // Encoded exactly as the interned form this callee may equally be handed: an offset from
        // the anchor, not the pointer this frame just computed. Being able to write that from a
        // frame at all is what the anchor is for - see repr/table.h.
        block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
            address, tableSlotValue(lower, block, value), kTableCellSize));
        index++;
    }

    return base;
}

/*
 * The implementation a deferred class dispatch resolves to.
 *
 * Two loads and no search: the witness out of the environment slot the schema numbered, and the
 * method out of the witness at the index the class declared. Which instance that witness belongs to
 * was decided by whoever built the environment, which is exactly the point - the body doing the
 * dispatching never learns what type it is dispatching on.
 *
 * One more load per superclass where the dispatch is on a requirement another one implies, which is
 * what genWitness walks: `Num(a)` in hand and `FromInt(a)` wanted is the `Num` witness, its `FromInt`
 * pointer, and then the method - still no search, and still nothing the caller had to pass twice.
 */
LowerPtr<LowerValue> genMethod(LowerContext& lower, LowerBlock& block, InstGenCall& call) {
    auto witness = genWitness(lower, block, call.classSlot, call.classPath);
    return tableSlotAddress(lower, block, witness, ClassWitnessFields::method(call.index));
}

/*
 * A field of a type this body cannot see - Implementation-Generics.md part 5's `PropertyWitness`.
 *
 * The third thing a place can end in that has no address, after a folded tag and a packed field, and
 * intercepted in the same two spots for the same reason. Here the reason is stronger: the owner is a
 * type variable, so there is no offset even in principle - where the field sits was decided by
 * whoever built the environment, and this body was compiled once for all of them.
 *
 * Returns the environment slot the constraint was numbered at, or maxLimit when the place does not
 * end in one. Only the *last* projection is answered: a path continuing through a property would
 * need the read's result to be an address, and what the witness hands back is a value in storage
 * this frame provided - so such a body specializes instead, which lowerablePlace enforces.
 */
U16 propertySlotOf(LowerContext& lower, const Place& place) {
    auto projections = place.projections;
    auto count = projections.size();
    if(!count) return maxLimit<U16>;

    auto last = projections.get(lower.local, count - 1);
    return last.kind == ProjectionKind::Property ? last.index : maxLimit<U16>;
}

// One operation of a property witness, loaded out of the witness the environment slot holds. Two
// loads and no search, exactly as a class method dispatch is - see genMethod.
LowerPtr<LowerValue> propertyOp(LowerContext& lower, LowerBlock& block, U16 slot, U16 field) {
    auto witness = genSlot(lower, block, slot);
    return tableSlotAddress(lower, block, witness, field);
}

// `op(owner, other)`, which is the shape both halves of a property witness have: two addresses in,
// nothing out. What differs between read and set is which address is the field's storage.
void callPropertyOp(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> op,
                           LowerPtr<LowerValue> owner, LowerPtr<LowerValue> other) {
    call(lower.lower, lower.to, block, 0, 3, kDefaultCallType, [&](LowerInstCall* invoke) {
        invoke->used()[0] = op;
        invoke->used()[1] = owner;
        invoke->used()[2] = other;
    });
}

/*
 * Zeroing fresh storage whose niche is made of bits nobody writes - see ReprTable::hasPaddedWord.
 *
 * The largest chunks first, then whatever is left, which for every type this is asked about is one or
 * two stores: a record of two `Bool`s is a byte. It is deliberately the whole allocation rather than
 * only the padded words - a second pass over the field list to find them would cost more here than the
 * stores save, and zero padding is the honest state for anything else that reads storage as bytes.
 */
void zeroStorage(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> address, U32 bytes) {
    U32 at = 0;

    while(at < bytes) {
        auto width = bytes - at >= 8 ? 8u : bytes - at >= 4 ? 4u : bytes - at >= 2 ? 2u : 1u;
        auto target = addOffset(lower, block, address, at);

        block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(target, immediate(lower, 0), width));
        at += width;
    }
}

// Storage for one value of a type this body may not know the size of - the erased counterpart of an
// ordinary alloca, taking its size from the caller's descriptor where the type is a variable.
LowerPtr<LowerValue> erasedStorage(LowerContext& lower, LowerBlock& block, TypePtr type,
                                          StringId name) {
    auto bytes = sizeOfType(lower, block, type);
    auto alignment = isGeneric(lower.global, type) ? 16u : typeAlign(lower, type);
    auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(name, bytes, alignment));

    return allocation->created().ptr - lower.lower;
}
