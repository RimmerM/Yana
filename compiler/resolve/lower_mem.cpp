/*
 * Storage and ownership: the instructions that move bytes rather than compute a value.
 *
 * Allocation, loads and stores through a place, the four ownership transfers - move, swap, exchange,
 * copy - and the drop that ends an owner. What they have in common is `lowerStore` and `relocate`:
 * every one of them ends up putting a value somewhere it was not, and whether that is an assignment,
 * a relocation hook or a call into a witness is the one decision they share.
 */

#include "lower_internal.h"

/*
 * Putting an aggregate's bytes somewhere they were not.
 *
 * Every such write in this pass is one of Design-Memory §4.1's two relocations, and which one was
 * settled in the resolver: a TrivialSink value is its bytes and moves as a block copy, and anything
 * else moves by the call InstMove::sink names - the authored `Sink` where the type has one, and the
 * member-wise glue where a member does. Both callees take the destination and then the source as
 * addresses and return nothing, which is why one call shape serves both - and why the erased case
 * below, whose callee was decided by the caller rather than by the resolver, is the same shape again.
 *
 * `value` is the resolve-level value being written, and it is the *value* rather than the type that
 * decides: only a move relocates. Initializing storage from a copy or from a call result is a write
 * of bytes that already belong to nobody else, and a sink would be wrong there - it would empty a
 * temporary that has no source to be emptied from.
 */
/*
 * The relocation itself, once the two questions above it have been answered: is this a relocation
 * at all, and which `Sink` does it run. Split out so that swap and exchange can ask for one
 * directly - they are relocations by construction, and the sink is on the instruction rather than
 * on a value that has to be recognized as a move first.
 */
LowerInst* relocateWith(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> target,
                               LowerPtr<LowerValue> source, TypePtr type) {
    /*
     * A relocation is the bytes. All of them, on this target, whatever the body can see.
     *
     * `sizeOfType` is what makes the erased case the same instruction as the concrete one: a
     * constant for a type this body knows and a load out of the caller's descriptor for a type
     * variable, so the only difference between the two is where the count comes from.
     *
     * It used to be a call through the descriptor's `moveInit` slot whenever the type was generic.
     * That slot answered *which* relocation applied, back when a type could supply an authored one -
     * see doc/spec/core.md#there-is-no-authored-relocation for why none can. With that gone the slot
     * held one thing for every type in the language, a block copy of the size sitting two cells
     * above it, and reaching it cost a load and an indirect call to reach a `memcpy` this emits
     * inline.
     *
     * **A managed target still reads the slot**, and that is not an inconsistency: a block copy
     * there is property by property, so what its erased relocation needs is the *shape*, and a byte
     * count is not one. See `erasedRelocate` in codegen/js/inst.cpp.
     */
    auto count = sizeOfType(lower, block, type);
    return block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(target, source, count));
}


/*
 * One write into one place - what an `Init`, an `Assign` and one element of an `InstAggregate` all
 * are by the time lowering sees them.
 *
 * Extracted so the aggregate goes through *this* rather than through a second opinion about niche
 * tags, packed fields, scalarized locals and property slots - eight paths, each of which an element
 * of a literal can land in. Every path that performs the write itself answers null, exactly as the
 * early returns did when this was inline; the one that produces a store returns it, because the
 * caller records it as the instruction's result.
 */
LowerInst* lowerStore(LowerContext& lower, LowerBlock& block, Function* function,
                             Place place, ModulePtr<Value> value) {

        /*
         * Writing a value that carries nothing writes nothing.
         *
         * The resolver skips this where it can see it - a field of unit type is never written -
         * but it cannot see it through a type variable, and a specialization at `{}` is a clone
         * of instructions decided before the substitution. `Empty {only: value}.only` is the
         * shape: the constructor's content is `a`, and writing it is an Init the generic body
         * had every reason to emit.
         */
        if(isUnit(lower.global, lower.local[value]->type)) return nullptr;

        if(isScalarPlace(lower, place)) {
            lower.scalars[place.local][scalarField(lower, place)] = mappedValue(lower, value);
            return nullptr;
        }

        // The other half of the shared expansion: the merged word arrives already computed, so
        // this is the store it was computed for. The unit's width rather than the value's, for
        // the reason unitBits gives.
        if(auto bits = unitBits(lower, place)) {
            auto address = lowerPlace(lower, block, *function, place);
            block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                address, mappedValue(lower, value), bits / 8));
            return nullptr;
        }

        /*
         * Writing a folded tag, which is a store of a pattern or nothing at all.
         *
         * The constructor is always a literal here: a record is constructed by naming one, and
         * `place.discriminant = <computed>` is not something any front end can write. An
         * assertion rather than a fallback, because a runtime encode would be dead code that
         * nothing could ever exercise or test.
         */
        if(auto record = foldedTagRecord(lower, *function, place)) {
            auto& written = *lower.local[value];
            assertTrue(written.kind == Value::ConstInt);

            auto payload = lowerPlace(lower, block, *function, place);
            encodeNicheTag(lower, block, payload, record, ((ConstInt&)written).value);
            return nullptr;
        }

        // A bit tag, written the way a co-packed field is: the word is read, the tag's bits are
        // replaced, and the payload sharing it comes back unchanged. Literal for the same reason
        // as above - nothing can write a computed constructor index.
        if(auto record = bitTagRecord(lower, *function, place)) {
            auto& written = *lower.local[value];
            assertTrue(written.kind == Value::ConstInt);

            auto word = lowerPlace(lower, block, *function, place);
            encodeBitTag(lower, block, word, record, ((ConstInt&)written).value);
            return nullptr;
        }

        /*
         * Writing a constrained field, which is the mirror image: the replacement goes into
         * storage of its own and the witness takes it from there.
         *
         * `set` consumes what it is handed, so this is a relocation into that storage rather
         * than a borrow of wherever the value already was - the callee commits it and releases
         * whatever the field held, and nothing here may release it a second time.
         */
        if(auto slot = propertySlotOf(lower, place); slot != maxLimit<U16>) {
            auto count = place.projections.size();
            auto owner = lowerPlace(lower, block, *function, place, count - 1);
            auto written = lower.local[value]->type;
            auto staging = erasedStorage(lower, block, written, StringId());
            auto staged = mappedValue(lower, value);

            if(isMemoryType(lower.global, written)) {
                relocateWith(lower, block, staging, staged, written);
            } else {
                block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                    staging, staged, memoryWidth(lower, written)));
            }

            callPropertyOp(lower, block, propertyOp(lower, block, slot, PropertyWitnessFields::kSet),
                           owner, staging);
            return nullptr;
        }

        // Written into a bit range rather than into storage. A scalar aggregate arrives as the
        // address of its own storage, so what goes into the word is a load of it - see
        // scalarBitsOf - and the merge masks it to the field's width either way.
        if(auto field = packedAccess(lower, *function, place); field.exists()) {
            auto word = lowerPlace(lower, block, *function, place);
            auto bits = scalarBitsOf(lower, block, lower.local[value]->type,
                                     mappedValue(lower, value));

            encodePackedField(lower, block, word, field, bits);
            return nullptr;
        }

        if(auto access = narrowRefAccess(lower, *function, place); access.exists()) {
            auto ref = unpackNarrowRef(lower, block, narrowRefValue(lower, *function, place),
                                       access);
            auto bits = scalarBitsOf(lower, block, lower.local[value]->type,
                                     mappedValue(lower, value));

            encodeNarrowRef(lower, block, ref, bits);
            return nullptr;
        }

        auto address = lowerPlace(lower, block, *function, place);
        auto stored = mappedValue(lower, value);

        if(isMemoryType(lower.global, lower.local[value]->type)) {
            return relocateWith(lower, block, address, stored, lower.local[value]->type);
        } else {
            // As at the load: a tag is as wide as its record's Repr says, and the constructor
            // index being written is an `Int` whichever record it belongs to.
            auto tagWidth = discriminantWidth(lower, *function, place);
            auto width = tagWidth ? tagWidth : memoryWidth(lower, lower.local[value]->type);

            return block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, stored, width));
        }

}

/*
 * A run whose every element is the same byte, as one `SetPattern` - the block fill.
 *
 * `[0] :: [U8 *64]` is a fill written as one element, and the resolver expands it into the same
 * sixty-four-component `InstAggregate` a hand-written literal produces, precisely so that nothing
 * between there and here needs a second shape to reason about. This is where it collapses again -
 * and it collapses a *hand-written* run of zeroes on the same terms, which is why the recognition is
 * over the aggregate rather than over the syntax that built it. A growable literal's run is the same
 * instruction over the same steps, so `[0, 0, ...] :: [Int]` reaches it too.
 *
 * Five things have to hold, and each is a fact this pass checks rather than a claim it is handed:
 *
 *  - **Every component is an element**, at index `i` in position `i`, covering the whole run. An
 *    aggregate with a constructor or a `Field` step is a record, whose components are not a stride
 *    apart; and a component `buildAggregate` skipped - which is what an element that failed to
 *    resolve leaves - shifts every index after it and is refused by the same test.
 *  - **An integer constant.** `SetPattern` names a byte, and a value known only at run time cannot
 *    be shown to have a uniform one however wide it is.
 *  - **The same constant in every element.** Compared by *value* rather than by pointer, which is
 *    what lets a hand-written run of zeroes collapse and not only a fill: the fill pushes one value
 *    `count` times, but `[0, 0, ...]` resolves each zero separately and nothing above here merges
 *    them. Two `ConstInt`s of one type holding one number are one byte pattern, which is the whole
 *    of what this needs to know about them.
 *  - **A plain scalar whose stride is its size.** A narrow repr - `Bool`, a three-bit enum - stores
 *    fewer bits than it occupies and its store path is not a plain one; a stride above the size
 *    would leave inter-element padding this writes and the stores do not.
 *  - **Every byte of the element equal.** `0` and `-1` qualify at every width and any `U8` does;
 *    `7 :: U32` does not, because `07 07 07 07` is not seven. Read off the constant rather than off
 *    the target's byte order, which does not enter: a value whose bytes are all equal reads the same
 *    either way, and one whose bytes are not is refused here.
 *
 * **How many bytes is not one of them**, which is the same answer `relocateWith` gives one line up:
 * a whole-value copy becomes a `LowerInstCopy` at every size, and what it turns into is the target's
 * business. `expandBlockOperations` walks both kinds in one pass against one policy - `setLimit` and
 * `copyLimit` are the same number, and `setStep` and `copyStep` are - so a fill below the ceiling is
 * unrolled to the widest transfer the machine has and one above it keeps the string instruction.
 * There is nothing left here for a size to decide, and a second threshold in front of that one would
 * be this file holding an opinion about a machine it cannot see.
 *
 * A run of one is still refused, and that is not a size rule: one element is already one store, so
 * there is no run to collapse and `expandSet` would produce the very same instruction.
 */
static bool lowerUniformFill(LowerContext& lower, LowerBlock& block, Function& function,
                             InstAggregate& aggregate) {
    if(aggregate.constructor != maxLimit<U16>) return false;

    auto count = aggregate.components.size();
    if(count < 2) return false;

    auto first = aggregate.components.get(lower.local, 0);
    if(!first.value) return false;

    auto value = lower.local[first.value];
    if(value->kind != Value::ConstInt) return false;

    auto repr = lower.repr.of(value->type);
    if(!repr.scalarBits || isNarrowRepr(repr) || !repr.size || repr.stride != repr.size) return false;

    auto written = ((ConstInt*)value)->value;
    auto byte = written & 0xff;

    for(U32 i = 1; i < repr.size; i++) {
        if(((written >> (i * 8)) & 0xff) != byte) return false;
    }

    for(Size i = 0; i < count; i++) {
        auto component = aggregate.components.get(lower.local, i);
        if(!component.value) return false;

        auto element = lower.local[component.value];
        if(element->kind != Value::ConstInt || ((ConstInt*)element)->value != written) return false;

        // The type as well as the number, because the byte count comes from the first element's
        // repr and a component of a different width would be filled at the wrong stride.
        if(element->type != value->type) return false;

        if(component.step.kind != ProjectionKind::Index || !component.step.value) return false;

        auto index = lower.local[component.step.value];
        if(index->kind != Value::ConstInt || ((ConstInt*)index)->value != i) return false;
    }

    auto to = lowerPlace(lower, block, function, aggregate.place);
    if(!to) return false;

    block.addInst(lower.lower, new (lower.to.arena) LowerInstSetPattern(
        to, immediate(lower, U64(repr.size) * count), immediate(lower, byte, LowerType::Int32)));

    return true;
}

/*
 * The storage and ownership instructions.
 *
 * Returns the instruction whose result is this value, or null when the arm mapped what it produced
 * itself - which every arm that emits more than one instruction, and every arm whose value is a
 * place rather than a computation, has to do.
 */
LowerInst* lowerStorageInst(LowerContext& lower, LowerBlock& block, Inst& instruction,
                            ModulePtr<Value> instValue, Function* function) {
    LowerInst* result = nullptr;

    switch(instruction.kind) {
        case Value::Alloc: {
            // Two of the four storage classes are ever selected - see StorageClass. Region and
            // Inline are asserted rather than silently treated as a frame slot, since a
            // region-placed value landing on the stack would be a lifetime bug rather than a slow
            // program.
            auto& allocation = (InstAlloc&)instruction;

            // A scalarized aggregate has no storage to create: its fields are values, and the
            // instructions that would have written and read them are rewritten below.
            if(allocation.local < lower.scalars.size() && lower.scalars[allocation.local].size()) {
                return nullptr;
            }

            /*
             * How much storage, and how much of it needs zeroing.
             *
             * A run of `n` slots is `n` strides rather than one size - see InstAlloc::extent. The
             * count is a value rather than a number, so this is a multiply that constant-folds to
             * the single immediate every other allocation gets whenever `n` is known here, which for
             * a literal's run it always is.
             */
            auto slotBytes = allocation.extent ? strideSize(lower, block, instruction.type)
                                               : storageSize(lower, block, instruction.type);
            auto bytes = slotBytes;
            auto zeroed = lower.repr.hasPaddedWord(instruction.type) ? lower.repr.sizeOf(instruction.type) : 0u;

            if(allocation.extent) {
                bytes = scaleBy(lower, block, slotBytes, mappedValue(lower, allocation.extent));

                // A niche in the *element* would need every slot zeroed rather than the first, which
                // is a loop rather than a store. Nothing produces one yet - a run's slots are
                // uninitialized by contract and written whole - so this reports rather than emitting
                // a zeroing that covers one element out of n.
                assertTrue(!zeroed);
            }

            if(allocation.storage == StorageClass::Heap) {
                // Storage escape analysis proved the frame cannot hold, so it comes from the
                // Native allocator instead. The release is an InstDrop the drop pass inserted,
                // or the new owner's - see InstAlloc::releasedHere.
                auto target = lower.functions.getValue(lower.from.allocateHeap).unwrap();
                auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(StringId(), target));

                result = call(lower.lower, lower.to, block, 1, 2, lower.lower[target]->callType,
                              [&](LowerInstCall* allocate) {
                    new (allocate->created().ptr) LowerValue(allocate, LowerType::Pointer, instruction.name);
                    allocate->used()[0] = fun->created().ptr - lower.lower;
                    allocate->used()[1] = bytes;
                });

                result->source = instruction.source;

                auto heap = result->created().ptr - lower.lower;
                if(zeroed) zeroStorage(lower, block, heap, zeroed);

                lower.values.add(instValue, heap);
                return nullptr;
            }

            assertTrue(allocation.storage == StorageClass::Stack);

            // An alloca states its alignment at compile time, and a generic body has no type to ask
            // for one. Over-aligning is always safe and costs a few bytes of frame, so the erased
            // path takes the widest alignment any target ABI asks for rather than loading the real
            // one and having no way to use it.
            auto alignment = isGeneric(lower.global, instruction.type)
                ? 16u : typeAlign(lower, instruction.type);

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, alignment));

            // A niche made of bits no field writes needs them to start out zero. Asked of the type
            // rather than of the allocation, so a generic body - whose type has no layout here - never
            // reaches it.
            if(zeroed) zeroStorage(lower, block, result->created().ptr - lower.lower, zeroed);

            break;
        }
        case Value::LoadPlace: {
            auto& loadInst = (InstLoadPlace&)instruction;

            // Reading a field that holds nothing produces nothing, so the load is not emitted at
            // all rather than emitted and left unmapped. `fn unbox(b: Box(a)) -> a = b.inner`
            // specialized at `{}` is the everyday way to reach this.
            if(isUnit(lower.global, instruction.type)) return nullptr;

            // Reading a field of a scalarized aggregate is naming the value that was written into
            // it, which is what makes the load disappear rather than become a cheaper load.
            if(isScalarPlace(lower, loadInst.place)) {
                lower.values.add(instValue, lower.scalars[loadInst.place.local][scalarField(lower, loadInst.place)]);
                return nullptr;
            }

            // A whole storage unit, which the shared expansion asked for by name: the shift and the
            // mask that used to follow are already instructions in front of this one, so all that is
            // left here is the load they read.
            if(auto bits = unitBits(lower, loadInst.place)) {
                auto address = lowerPlace(lower, block, *function, loadInst.place);
                lower.values.add(instValue, load(
                    lower.lower, lower.to, block, lower.lower[address], bits / 8, false,
                    lowerType(lower, instruction.type), instruction.name
                )->created().ptr - lower.lower);
                return nullptr;
            }

            // A folded tag is not in memory to be loaded - see decodeNicheTag. The place still
            // lowers to the payload's address, since a folded record's payload begins where the
            // record does.
            if(auto record = foldedTagRecord(lower, *function, loadInst.place)) {
                auto payload = lowerPlace(lower, block, *function, loadInst.place);
                lower.values.add(instValue, decodeNicheTag(lower, block, payload, record,
                                                           instruction.type, instruction.name));
                return nullptr;
            }

            // A bit tag is in memory, but not at a width its type describes - see decodeBitTag.
            if(auto record = bitTagRecord(lower, *function, loadInst.place)) {
                auto word = lowerPlace(lower, block, *function, loadInst.place);
                lower.values.add(instValue, decodeBitTag(lower, block, word, record,
                                                         instruction.type, instruction.name));
                return nullptr;
            }

            /*
             * A constrained field, read through the witness the caller passed.
             *
             * The result goes into storage this frame provides, for the same reason every erased
             * result does: the field's type is a variable, so there is no register class to hand it
             * back in. A scalar is then loaded straight back out of it, which is what keeps a
             * specialized body and an erased one saying the same thing about the value.
             */
            if(auto slot = propertySlotOf(lower, loadInst.place); slot != maxLimit<U16>) {
                auto count = loadInst.place.projections.size();
                auto owner = lowerPlace(lower, block, *function, loadInst.place, count - 1);
                auto out = erasedStorage(lower, block, instruction.type, instruction.name);

                callPropertyOp(lower, block, propertyOp(lower, block, slot, PropertyWitnessFields::kRead),
                               owner, out);

                if(isMemoryType(lower.global, instruction.type)) {
                    lower.values.add(instValue, out);
                    return nullptr;
                }

                lower.values.add(instValue, load(
                    lower.lower, lower.to, block, lower.lower[out],
                    memoryWidth(lower, instruction.type),
                    signedType(lower.global, instruction.type),
                    lowerType(lower, instruction.type),
                    instruction.name
                )->created().ptr - lower.lower);
                return nullptr;
            }

            // A packed field is not at an address either, and the place walk has already produced
            // the address of the word it lives in - see packedAccess.
            if(auto field = packedAccess(lower, *function, loadInst.place); field.exists()) {
                auto word = lowerPlace(lower, block, *function, loadInst.place);

                // A scalar aggregate read out of a word needs storage of its own, because every
                // consumer of an aggregate takes an address. Everything narrower than that is a
                // value, and arrives in a register the way it always did.
                if(isMemoryType(lower.global, instruction.type)) {
                    auto bits = decodePackedBits(lower, block, word, field,
                                                 signedType(lower.global, instruction.type));
                    lower.values.add(instValue, materializeScalar(lower, block, instruction.type, bits,
                                                                  instruction.name));
                    return nullptr;
                }

                lower.values.add(instValue, decodePackedField(lower, block, word, field,
                                                              instruction.type, instruction.name));
                return nullptr;
            }

            // Reading through a reference that carries a shift, where the shift is the caller's
            // rather than a constant this body knows - see NarrowRef.
            if(auto access = narrowRefAccess(lower, *function, loadInst.place); access.exists()) {
                auto ref = unpackNarrowRef(lower, block, narrowRefValue(lower, *function, loadInst.place),
                                           access);

                if(isMemoryType(lower.global, instruction.type)) {
                    lower.values.add(instValue, materializeScalar(lower, block, instruction.type,
                                                                  decodeNarrowBits(lower, block, ref),
                                                                  instruction.name));
                    return nullptr;
                }

                lower.values.add(instValue, decodeNarrowRef(lower, block, ref, instruction.type,
                                                            instruction.name));
                return nullptr;
            }

            auto address = lowerPlace(lower, block, *function, loadInst.place);

            // An aggregate is never loaded into a value: the address of its storage is what the
            // rest of the lowering uses in its place.
            if(isMemoryType(lower.global, instruction.type)) {
                lower.values.add(instValue, address);
                return nullptr;
            }

            // A tag takes its width from the record it belongs to rather than from the `Int` the
            // projection produces - see discriminantWidth.
            auto tagWidth = discriminantWidth(lower, *function, loadInst.place);

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                tagWidth ? tagWidth : memoryWidth(lower, instruction.type),
                signedType(lower.global, instruction.type),
                lowerType(lower, instruction.type),
                instruction.name
            );

            /*
             * The overread flag, carried across the seam - see InstLoadPlace::overread.
             *
             * Only on this path, and that is the whole of its meaning: every path above it produces
             * something other than a load of the place's own address - a scalarized value, a decoded
             * tag, a witness call - and none of those reads past anything. A flag that survived into
             * one of them would be a claim about an instruction that is not there.
             */
            if(loadInst.overread) ((LowerInstLoad*)result)->setOverread();
            break;
        }
        case Value::Init:
        case Value::Assign: {
            // The two are one instruction here. Whatever the old value's drop needed has already
            // been emitted as its own InstDrop by the drop pass, so by the time lowering sees an
            // assignment there is nothing left in it but the write.
            auto& init = (InstInit&)instruction;

            // Null where the write was performed by one of `lowerStore`'s own paths - a scalarized
            // local, a packed field, a witness call. Those produced no instruction to map a result
            // to, which is what the early returns here used to say.
            result = lowerStore(lower, block, function, init.place, init.value);
            if(!result) return nullptr;
            break;
        }
        /*
         * The elements of a literal, expanded here into the stores the native target wanted all
         * along - see InstAggregate for why they arrive as one instruction instead.
         *
         * Element `i` is the run's place with an `Index i` projection appended, which is the form
         * `[T *n]` introduced and the one `lowerPlace` already turns into `base + i * stride`. The
         * index is a constant, so the multiply folds and what reaches the backend is a store at a
         * displacement - the same instruction the per-element writes produced.
         */
        case Value::Aggregate: {
            auto& aggregate = (InstAggregate&)instruction;

            if(lowerUniformFill(lower, block, *function, aggregate)) return nullptr;

            eachWrittenComponent(lower.local, lower.from.arena, aggregate,
                                 [&](Place place, ModulePtr<Value> value, Size) {
                lowerStore(lower, block, function, place, value);
            });

            // Nothing to map: the instruction is a statement of unit type whose elements each
            // produced their own store, so there is no single result any of them stands for.
            return nullptr;
        }
        case Value::Borrow: {
            /*
             * A borrow is the address of what it borrows. Nothing is loaded and nothing is copied,
             * which is the whole of what "non-owning, zero-cost" means once the checking is done.
             *
             * Unless what it borrows is narrow, in which case it is that address plus the shift of
             * the field within the unit it names - Design.md's tier 2. Both halves are constants
             * here: the borrow site knows exactly which field it is taking, so the arithmetic below
             * folds to nothing at all for an unpacked one.
             */
            auto& borrow = (InstBorrow&)instruction;
            auto referenced = ((BorrowType*)lower.global[instruction.type])->to;

            if(!isNarrowRepr(lower.repr.of(referenced))) {
                lower.values.add(instValue, lowerPlace(lower, block, *function, borrow.place));
                return nullptr;
            }

            auto unitBytes = naturalStorageBits(lower.repr.of(referenced).scalarBits) / 8;

            // Reborrowing one: it is already a reference of this shape and carries its own shift, so
            // a reference to a *field* of what it names is the same word with the field's offset
            // added - and the total re-split against the unit the new pointee is loaded in.
            if(auto access = narrowRefAccess(lower, *function, borrow.place); access.exists()) {
                auto held = narrowRefValue(lower, *function, borrow.place);
                if(!access.bitOffset && unitBytes * 8 == naturalStorageBits(access.bitWidth)) {
                    lower.values.add(instValue, held);
                    return nullptr;
                }

                auto ref = unpackNarrowRef(lower, block, held, access);
                lower.values.add(instValue, stepNarrowRef(lower, block, ref, unitBytes));
                return nullptr;
            }

            auto address = lowerPlace(lower, block, *function, borrow.place);
            U32 shift = 0;

            /*
             * A packed field, whose word the place walk has already produced the address of. What is
             * left is to step to the *unit* the field sits in - which it never straddles, by the
             * placement rule packBits applies - and to record where inside it the field starts.
             */
            if(auto field = packedAccess(lower, *function, borrow.place); field.exists()) {
                auto unit = unitBytes * 8;
                auto index = field.bitOffset / unit;

                address = addOffset(lower, block, address, index * unitBytes);
                shift = field.bitOffset - index * unit;
            }

            lower.values.add(instValue, packNarrowRef(lower, block, address, shift));
            return nullptr;
        }
        case Value::Move: {
            /*
             * A move on its own needs no code at all: the bytes stay where they are and what
             * changed is only which name is allowed to reach them. The instruction produces the
             * source's address, and every consumer that would have taken a copy of those bytes -
             * an initialization, an assignment, a return into the caller's result slot - goes
             * through relocate() instead, which is where InstMove::sink turns into a call.
             *
             * A consumer that takes the address and nothing else is right to emit nothing: passing
             * a `->` argument hands the callee the storage it already sits in, and a value that was
             * never relocated has nothing to relocate it with.
             */
            auto& moved = (InstMove&)instruction;
            auto address = lowerPlace(lower, block, *function, moved.place);

            if(isMemoryType(lower.global, instruction.type)) {
                lower.values.add(instValue, address);
                return nullptr;
            }

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                memoryWidth(lower, instruction.type),
                signedType(lower.global, instruction.type),
                lowerType(lower, instruction.type),
                instruction.name
            );
            break;
        }
        case Value::Swap: {
            /*
             * Three relocations through a temporary, and the temporary is not removable: neither
             * place can be written until both have been read, so something has to hold the first
             * one while the second is written over it. That is the cost `exchange` exists to avoid.
             *
             * A scalar needs no storage for it - two loads before either store is the same
             * statement, said in registers.
             */
            auto& swap = (InstSwap&)instruction;

            /*
             * Both sides through references that carry a shift, where the two may be bit ranges of one
             * word. Read before either write, which is what a swap is; the writes are then ordinary
             * read-modify-writes, and the second reads what the first wrote - so two fields of one word
             * exchange without either losing the other, for the same reason two commits do.
             */
            if(auto accessA = narrowRefAccess(lower, *function, swap.a); accessA.exists()) {
                auto accessB = narrowRefAccess(lower, *function, swap.b);
                if(accessB.exists()) {
                    auto refA = unpackNarrowRef(lower, block, narrowRefValue(lower, *function, swap.a),
                                                accessA);
                    auto refB = unpackNarrowRef(lower, block, narrowRefValue(lower, *function, swap.b),
                                                accessB);

                    auto oldA = decodeNarrowBits(lower, block, refA);
                    auto oldB = decodeNarrowBits(lower, block, refB);

                    encodeNarrowRef(lower, block, refA, oldB);
                    encodeNarrowRef(lower, block, refB, oldA);
                    return nullptr;
                }
            }

            auto a = lowerPlace(lower, block, *function, swap.a);
            auto b = lowerPlace(lower, block, *function, swap.b);
            auto erased = lower.genEnv && isGeneric(lower.global, swap.content);

            if(!isMemoryType(lower.global, swap.content)) {
                auto width = memoryWidth(lower, swap.content);
                auto isSigned = signedType(lower.global, swap.content);
                auto kind = lowerType(lower, swap.content);

                auto oldA = load(lower.lower, lower.to, block, lower.lower[a], width, isSigned, kind, StringId());
                auto oldB = load(lower.lower, lower.to, block, lower.lower[b], width, isSigned, kind, StringId());

                block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                    a, oldB->created().ptr - lower.lower, width));

                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                    b, oldA->created().ptr - lower.lower, width));

                break;
            }

            auto bytes = sizeOfType(lower, block, swap.content);
            auto alignment = isGeneric(lower.global, swap.content) ? 16u : typeAlign(lower, swap.content);
            auto temporary = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(StringId(), bytes, alignment));
            auto slot = temporary->created().ptr - lower.lower;

            relocateWith(lower, block, slot, a, swap.content);
            relocateWith(lower, block, a, b, swap.content);
            result = relocateWith(lower, block, b, slot, swap.content);
            break;
        }
        case Value::Exchange: {
            /*
             * Two relocations and no temporary. What is coming in is already a value rather than a
             * place - the caller moved it in - so there is nothing to save from being written over,
             * and the storage the old contents leave for is the result's own.
             */
            auto& exchange = (InstExchange&)instruction;
            auto incoming = mappedValue(lower, exchange.value);
            auto content = instruction.type;

            // Through a reference that carries a shift: read the old bits out, write the new ones in,
            // and hand back what was there. A scalar aggregate needs storage for the result, exactly as
            // an ordinary read of one does.
            if(auto access = narrowRefAccess(lower, *function, exchange.place); access.exists()) {
                auto ref = unpackNarrowRef(lower, block, narrowRefValue(lower, *function, exchange.place),
                                           access);
                auto old = decodeNarrowBits(lower, block, ref);
                encodeNarrowRef(lower, block, ref, scalarBitsOf(lower, block, content, incoming));

                if(isMemoryType(lower.global, content)) {
                    lower.values.add(instValue, materializeScalar(lower, block, content, old,
                                                                  instruction.name));
                    return nullptr;
                }

                result = ref.isSigned
                    ? cast<true, true>(lower.lower, lower.to, block, lower.lower[old],
                                       lowerType(lower, content), instruction.name)
                    : cast<false, false>(lower.lower, lower.to, block, lower.lower[old],
                                         lowerType(lower, content), instruction.name);
                break;
            }

            auto address = lowerPlace(lower, block, *function, exchange.place);

            if(!isMemoryType(lower.global, content)) {
                auto width = memoryWidth(lower, content);
                auto old = load(lower.lower, lower.to, block, lower.lower[address], width,
                                signedType(lower.global, content), lowerType(lower, content),
                                instruction.name);

                block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, incoming, width));
                result = old;
                break;
            }

            auto bytes = sizeOfType(lower, block, content);
            auto alignment = isGeneric(lower.global, content) ? 16u : typeAlign(lower, content);
            auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                instruction.name, bytes, alignment));

            auto target = allocation->created().ptr - lower.lower;
            auto erased = lower.genEnv && isGeneric(lower.global, content);

            // Out first, then in: the incoming value is relocated by whatever rule its own move
            // recorded, which is the same question an Init of it would ask and is asked the same way.
            relocateWith(lower, block, target, address, content);
            relocateWith(lower, block, address, incoming, content);

            lower.values.add(instValue, target);
            return nullptr;
        }
        case Value::Copy: {
            // A copy is a real duplicate, so unlike a move it needs storage of its own: an
            // aggregate is a block copy into a fresh alloca and a scalar is an ordinary load,
            // which is already a fresh value in a register.
            auto& copied = (InstCopy&)instruction;
            assertTrue(copied.copy == nullptr);

            auto address = lowerPlace(lower, block, *function, copied.place);

            if(isMemoryType(lower.global, instruction.type)) {
                auto bytes = storageSize(lower, block, instruction.type);
                auto alignment = isGeneric(lower.global, instruction.type)
                    ? 16u : typeAlign(lower, instruction.type);

                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                    instruction.name, bytes, alignment));

                auto target = allocation->created().ptr - lower.lower;
                auto count = sizeOfType(lower, block, instruction.type);
                auto blockCopy = block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(target, address, count));
                blockCopy->source = instruction.source;

                lower.values.add(instValue, target);
                return nullptr;
            }

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                memoryWidth(lower, instruction.type),
                signedType(lower.global, instruction.type),
                lowerType(lower, instruction.type),
                instruction.name
            );
            break;
        }
        case Value::Drop: {
            /*
             * A teardown to run and storage to hand back, either of which may be absent. A drop with
             * neither is one the pass should have elided rather than emitted.
             *
             * The order is the one the language requires - whatever the teardown does, it does while
             * the storage is still there to do it in.
             */
            auto& dropped = (InstDrop&)instruction;
            assertTrue(!dropped.isEmpty());

            auto address = lowerPlace(lower, block, *function, dropped.place);

            auto callWith = [&](ModulePtr<Function> callee) {
                auto target = lower.functions.getValue(callee).unwrap();
                auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(StringId(), target));

                return call(lower.lower, lower.to, block, 0, 2, lower.lower[target]->callType,
                            [&](LowerInstCall* dropCall) {
                    dropCall->used()[0] = fun->created().ptr - lower.lower;
                    dropCall->used()[1] = address;
                });
            };

            auto step = [&](ModulePtr<Function> callee) {
                if(!callee) return;
                if(result) result->source = instruction.source;
                result = callWith(callee);
            };

            /*
             * The erased case: a teardown the body cannot name, because the type it belongs to is
             * one of this function's own type variables. What runs is whatever the caller's
             * descriptor holds, reached through the same indirect call any function value uses -
             * the lower IR takes a call's callee as an ordinary operand, so a loaded address is as
             * good a callee as a symbol.
             *
             * **One call and not two.** Both halves live in one slot here (NativeTypeDesc::kTeardown),
             * already merged by whoever wrote the descriptor - so the "did the two halves name the
             * same walk" question the concrete path asks below is settled before this code runs,
             * rather than being a comparison of two loaded addresses that this call would have to
             * make on every drop of every erased value.
             *
             * The slot is never null, which is what keeps this branch-free: a type with nothing to
             * run gets the shared empty teardown rather than a hole, so the erased path is one
             * unconditional call instead of a load, a test and a split block. The flags in the
             * descriptor still say whether it is that empty one, for a later pass that wants to
             * skip the call rather than make it.
             *
             * `releaseStorage` is unaffected and is handled below with the concrete case: handing
             * an allocation back is this frame's business and needs no descriptor, since what
             * `freeHeap` is given is an address rather than a value.
             */
            if(dropped.erased) {
                auto placeType = dropPlaceType(lower, *function, dropped.place);
                auto descriptor = genTypeDesc(lower, block, placeType);

                // An erased drop is placed only where the type is one of this body's own variables,
                // so the descriptor its caller passed is exactly the one that describes it. Reaching
                // here without one means the place type could not be answered - `dropPlaceType` is
                // where that is decided - and the alternative to saying so out loud is indexing a
                // table at address zero, which is what a write through a `&` used to do here.
                assertTrue(bool(descriptor));

                auto operation = tableSlotAddress(lower, block, descriptor, NativeTypeDesc::kTeardown);

                result = call(lower.lower, lower.to, block, 0, 2, kDefaultCallType,
                              [&](LowerInstCall* teardown) {
                    teardown->used()[0] = operation;
                    teardown->used()[1] = address;
                });
            }

            /*
             * One call, because a drop names one function.
             *
             * This used to be two, guarded by whether the second named the same thing as the first -
             * a container writes a single walk over its live elements and supplies both halves from
             * it (Implementation-Containers.md §13), so running the second call would have released
             * every element twice. That comparison is a fact about the *type*, so it is made where
             * the type is (teardownBothFor) and what arrives here is the answer.
             */
            step(dropped.teardown);

            // Handing back this allocation is the last thing that happens to it, after both halves
            // have finished reading it.
            if(dropped.releaseStorage) step(lower.from.freeHeap);

            break;
        }
        case Value::Address: {
            // Nothing is loaded: the address the place computes is the value.
            auto address = lowerPlace(lower, block, *function, ((InstAddress&)instruction).place);
            lower.values.add(instValue, address);
            return nullptr;
        }
        default:
            assertTrue("unexpected instruction kind for this lowering" == nullptr);
            return nullptr;
    }

    return result;
}
