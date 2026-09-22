#include "internal.h"
#include "../../resolve_mir/specialize.h"

namespace ssa {

namespace {

/*
 * What an operation's *arithmetic* is at - the lane where the operand is a vector, and the operand
 * itself everywhere else.
 *
 * The one question a vector changes about the two functions below, and it changes it the same way
 * for both: a division of two `Vec(I32)` is a signed division per lane and a comparison of them is a
 * signed comparison per lane, so what decides the instruction is the lane's own type. Written once
 * because the two used to ask it separately and a vector answered "not signed" to both - `v / 3`
 * came out unsigned and `lane(v, i) < 0` came out false.
 */
static DeclPtr<mir::Type> arithmeticLane(DeclBase decl, DeclPtr<mir::Type> type) {
    if(auto lane = mir::laneTypeOf(decl, type)) return lane;
    return type;
}

/*
 * Which machine operation an SSA operation over this type becomes.
 *
 * The one place signedness enters the pipeline, which is why op.def has no column for it: `I32` and
 * `U32` are two logical types over one `div`, and it is here - with the type in hand and the target
 * decided - that the two become `idiv` and `div`. The same reading makes `<` two condition codes and
 * `+` one instruction.
 */
static LowerInst::Kind binaryKind(DeclBase decl, mir::BinaryOp op, DeclPtr<mir::Type> operandType) {
    auto& logical = *decl[arithmeticLane(decl, operandType)];
    auto floating = logical.kind == mir::Type::Float;
    auto isSigned = logical.kind == mir::Type::Int && ((mir::IntType&)logical).isSigned;

    switch(op) {
        case mir::BinaryOp::Add: return LowerInst::Add;
        case mir::BinaryOp::Sub: return LowerInst::Sub;
        case mir::BinaryOp::Mul: return floating || !isSigned ? LowerInst::Mul : LowerInst::IMul;
        case mir::BinaryOp::Div: return floating || !isSigned ? LowerInst::Div : LowerInst::IDiv;
        case mir::BinaryOp::Rem: return isSigned ? LowerInst::IRem : LowerInst::Rem;

        // `Mul`'s reading at the other half of the product - the lower IR has had this pair since
        // the reciprocal in `lower_strength.cpp` needed it, and this is the same pair reached from
        // source. Floats do not get here: `OpCategory::Integer` is the row's.
        case mir::BinaryOp::MulHi: return isSigned ? LowerInst::IMulHi : LowerInst::MulHi;

        // `bzhi`'s operation, which crosses this seam as itself - the widths it is defined at are
        // `hasMachineWidth`'s, so there is no narrow value in a register for signedness to mean
        // anything about.
        case mir::BinaryOp::BitsUpTo: return LowerInst::BitsUpTo;

        // The two saturating lane operations, which are `Div`'s reading a third time: which two
        // bounds a lane clamps to is exactly the question this seam exists to answer.
        case mir::BinaryOp::SaturatingAdd: return isSigned ? LowerInst::ISatAdd : LowerInst::SatAdd;
        case mir::BinaryOp::SaturatingSub: return isSigned ? LowerInst::ISatSub : LowerInst::SatSub;

        // `pext` and `pdep`, which cross this seam as themselves for `bzhi`'s reason: the widths
        // they are defined at are `hasMachineWidth`'s, so there is no narrow value in a register for
        // signedness to mean anything about.
        case mir::BinaryOp::GatherBits:  return LowerInst::GatherBits;
        case mir::BinaryOp::ScatterBits: return LowerInst::ScatterBits;
        case mir::BinaryOp::And: return LowerInst::And;
        case mir::BinaryOp::Or:  return LowerInst::Or;
        case mir::BinaryOp::Xor: return LowerInst::Xor;
        case mir::BinaryOp::Shl: return LowerInst::Shl;
        case mir::BinaryOp::Shr: return LowerInst::Shr;
        case mir::BinaryOp::Sar: return LowerInst::Sar;
        case mir::BinaryOp::Rol: return LowerInst::Rol;
        case mir::BinaryOp::Ror: return LowerInst::Ror;

        // The comparisons are one instruction with a condition code - see `lowerCmp`.
        case mir::BinaryOp::Eq:
        case mir::BinaryOp::Ne:
        case mir::BinaryOp::Lt:
        case mir::BinaryOp::Le:
        case mir::BinaryOp::Gt:
        case mir::BinaryOp::Ge:
        case mir::BinaryOp::Count:
            break;
    }

    return LowerInst::Cmp;
}

static LowerCmp lowerCmp(DeclBase decl, mir::BinaryOp op, DeclPtr<mir::Type> operandType) {
    auto& logical = *decl[arithmeticLane(decl, operandType)];
    auto isSigned = logical.kind == mir::Type::Int && ((mir::IntType&)logical).isSigned;

    switch(op) {
        case mir::BinaryOp::Eq: return LowerCmp::eq;
        case mir::BinaryOp::Ne: return LowerCmp::neq;

        // A float ordering is the unsigned code: the machine's ordered float compare is what the
        // backend selects from the operand type, and the signed codes are the integer ones.
        case mir::BinaryOp::Lt: return isSigned ? LowerCmp::ilt : LowerCmp::lt;
        case mir::BinaryOp::Le: return isSigned ? LowerCmp::ile : LowerCmp::le;
        case mir::BinaryOp::Gt: return isSigned ? LowerCmp::igt : LowerCmp::gt;
        case mir::BinaryOp::Ge: return isSigned ? LowerCmp::ige : LowerCmp::ge;
        default: break;
    }

    return LowerCmp::eq;
}

} // namespace

/*
 * One allocation on the heap - `StorageClass::Heap`, at the point the frame slot would have been.
 *
 * The **stride** and not the size, for `createBox`'s reason one line over: a record's size stops at
 * its last field so that a container can land in its trailing padding, and an allocation of its own
 * has no container to do that. The two heap sites therefore ask the same question and get the same
 * number, which is what keeps them one rule.
 *
 * A size that is not a number is `sizeValue`'s, which is the same computation `dynamicStorage` hands
 * an `alloca` - so a generic body compiled once places its heap objects exactly as it places its
 * frame ones, with the descriptor answering the width.
 */
LowerPtr<LowerValue> Lowering::heapStorage(LowerBlock& block, const mir::Occurrence& occurrence,
                                           LocationId source) {
    auto callee = roleFunction(intrinsic::Known::allocateHeap, source);
    if(!callee) return nullptr;

    auto manifest = manifestNative(layout, occurrence);
    auto bytes = manifest.dynamic
        ? sizeValue(block, occurrence)
        : immediate(block, LowerType::Int64, U64(strideOf(manifest) ? strideOf(manifest) : 1));

    auto name = block.addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

    auto allocated = call(lower, to, block, 1, 2, lower[callee]->callType,
                          [&](LowerInstCall* built) {
        new (built->created().ptr) LowerValue(built, LowerType::Pointer, StringId());
        built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
        built->used()[1] = bytes;
    });

    auto address = allocated->created().ptr - lower;

    /*
     * Zeroed on the same rule a frame slot and a box are - `hasPaddedWord`.
     *
     * `allocateHeap` hands back whatever was in the block, and a record with a co-packed word or a
     * provenance suffix reads those bits as its own starting state. This is the third allocation
     * site in the stage and therefore the third statement of the rule; there is no fourth.
     */
    if(manifest.dynamic || !manifest.inMemory || !hasPaddedWord(layout, occurrence.type)) {
        return address;
    }

    zeroStorage(block, address, manifest.size);
    return address;
}

LowerPtr<LowerValue> Lowering::frameStorage(LowerBlock& block, const mir::Occurrence& occurrence,
                                            bool whole) {
    auto manifest = manifestNative(layout, occurrence);

    // A size that is not a number, which is a type parameter in a body compiled once - `erase.cpp`,
    // where the alignment it settles for is decided and explained.
    if(manifest.dynamic) return dynamicStorage(block, occurrence);

    // A byte count as a value rather than a field, which is what `LowerInstAlloca` takes: an
    // allocation whose size is computed is the same instruction as one whose size is a literal, and
    // a generic type's size is exactly that computation.
    auto size = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), LowerType::Int64, U64(manifest.size ? manifest.size : 1)));

    auto value = block.addInst(lower, new (to.arena) LowerInstAlloca(
        StringId(), ((LowerInstSingle*)size)->created().ptr - lower,
        manifest.align ? manifest.align : 1));

    auto storage = ((LowerInstSingle*)value)->created().ptr - lower;

    /*
     * Zeroed where the type has a word whose spare bits nobody writes - `hasPaddedWord`, which is
     * where the reason is.
     *
     * Here rather than at each construction, because what needs it is the *storage* and not the
     * value that goes into it: a co-packed word is filled by read-modify-writes, so whichever of
     * them runs first inherits whatever was in the bits above the run - and the niche the record
     * publishes is made of exactly those bits.
     *
     * Not where the caller says the first write is a whole one - a decoded word stored at the
     * manifestation's width. The zeroing is then a write of bytes the very next instruction
     * replaces.
     *
     * A duplicate's slot is *not* one of those, although its copy writes every byte: the fill is
     * what gives the slot the word boundaries `splitAggregateSlots` cuts it at, and a temporary
     * with none is one cell that stays a block move. `forwardStoredValues` removes the fill once
     * the split has used it.
     */
    if(whole || !manifest.inMemory || !hasPaddedWord(layout, occurrence.type)) return storage;

    zeroStorage(block, storage, manifest.size);
    return storage;
}

/*
 * `size` bytes of storage written to zero, as one fill.
 *
 * Four callers, and they are the places a value with a padded word comes to occupy storage:
 * `frameStorage` and `heapStorage` above, where the storage is fresh, `destinationFor`'s `Store`
 * arm, where it is a slot the value is about to be written into anyway, and `createBox`. One
 * function because the rule they are serving is one rule - see `hasPaddedWord` - and two spellings
 * of it is how one of them comes to stop at four bytes.
 *
 * **A `setpattern` rather than a store per word**, which it was. The fill is what the zeroing *is*,
 * and every consumer below has a better answer for it than for a run of stores:
 * `expandBlockOperations` writes a short one as the widest transfers the machine has, LLVM reads
 * `llvm.memset`, `forwardStoredValues` sees one write that a copy of the whole value replaces, and
 * the two passes that turn slots into registers take a zero fill for the zero it writes -
 * `promoteStackSlots` over a whole slot, `splitAggregateSlots` cell by cell.
 */
void Lowering::zeroStorage(LowerBlock& block, LowerPtr<LowerValue> storage, U32 size) {
    if(!size) return;

    block.addInst(lower, new (to.arena) LowerInstSetPattern(
        storage, immediate(block, LowerType::Int64, size), immediate(block, LowerType::Int32, 0)));
}

/*
 * How many projections deep `allocationRoot` follows before answering "somewhere else".
 *
 * A bound rather than a limit: the chains this asks about are a field of a field of a container
 * header, three steps at the outside, and a walk with no bound would be one whose cost a body could
 * choose. Beyond it the answer is the conservative one, which is what every caller wants anyway.
 */
constexpr U32 kRootDepth = 8;

/*
 * The allocation an address is a projection of, and null where the walk leaves this frame.
 *
 * Only what `destinationFor` needs it for, which is a **separation** test and not a general one:
 * two addresses with two different roots cannot name one piece of storage, and an address with no
 * root here names storage this frame cannot bound. So the null answer is the conservative one at
 * every caller, and the walk stops rather than guessing at the first step it does not recognize.
 *
 * A `Load` is a step, and it is the only one that is not arithmetic: a `Guaranteed` result is a
 * borrow of what is at the address, so its storage is that address's storage - which is exactly what
 * `install` relies on when it copies out of one. A tracked load is a handover and is not followed.
 */
static SsaPtr<Value> allocationRoot(SsaBase ssa, SsaPtr<Value> pointer) {
    for(U32 step = 0; step < kRootDepth; step++) {
        if(!pointer) return nullptr;

        auto value = ssa[pointer];
        if(!value->definer) return nullptr;

        auto definer = ssa[value->definer];
        switch(definer->kind) {
            case Inst::Alloc:
                return pointer;

            case Inst::FieldAddress:
                pointer = ((InstFieldAddress*)definer)->base.value();
                break;

            case Inst::ElementAddress:
                pointer = ((InstElementAddress*)definer)->base.value();
                break;

            case Inst::Load:
                if(value->convention != Convention::Guaranteed) return nullptr;
                pointer = ((InstLoad*)definer)->address.value();
                break;

            default:
                return nullptr;
        }
    }

    return nullptr;
}

/*
 * Whether `consumer` is the instruction directly after `definer`, both being in `block`.
 *
 * A scan, because an `InstId` is a body-wide identity and says nothing about position - see
 * `Body::insts`. It runs only for a value whose one consumer is a `store`, and it stops at the
 * producer, so what it costs is bounded by the distance between the two instructions it is asking
 * about rather than by the block.
 */
static bool directlyFollows(SsaBase ssa, Block& block, SsaPtr<Inst> definer,
                            SsaPtr<Inst> consumer) {
    auto count = block.instructionCount();

    for(Size i = 0; i + 1 < count; i++) {
        if(block.instructionAt(ssa, i) == definer) {
            return block.instructionAt(ssa, i + 1) == consumer;
        }
    }

    return false;
}

/*
 * The address a memory value's consumer is going to want it at - see `destinationFor` in internal.h,
 * where the argument is.
 *
 * Four conditions, and each of them is what makes the rewrite a placement rather than a guess:
 *
 *  - the value **lives in memory** and its size is a number. A register value has no storage to
 *    place, and a dynamic one is `erase.cpp`'s business.
 *  - it has **exactly one use**. Two consumers cannot both own the storage, and a value that is also
 *    read somewhere else is one whose contents outlive the consumer that would take it over.
 *  - the use is in the **same block** as the definition, which is what makes the destination's
 *    address available where the producer writes without a dominance question being asked.
 *  - the type has **no padded word**. `frameStorage` zeroes such a slot before anything writes it,
 *    because a co-packed word and a box provenance byte are filled by read-modify-writes; placing
 *    the value in somebody else's storage would inherit whatever that storage held. Declining is one
 *    test where zeroing the destination would be a second copy of the rule.
 */
LowerPtr<LowerValue> Lowering::destinationFor(LowerBlock& block, Value& value) {
    auto manifest = manifestOf(value);
    if(!manifest.inMemory || manifest.dynamic) return nullptr;
    if(value.useCount() != 1 || !value.definer) return nullptr;

    auto padded = hasPaddedWord(layout, value.occurrence.type);
    auto consumer = ssa[value.useAt(ssa, 0)];
    if(consumer->block != ssa[value.definer]->block) return nullptr;

    switch(consumer->kind) {
        /*
         * The caller's hidden result pointer, which is the destination the whole convention exists
         * to name. Only where the `return` carries this one value: a result list with more than one
         * memory value in it would have two claims on one pointer.
         */
        case Inst::Return: {
            auto& written = (InstReturn&)*consumer;
            if(padded || !currentReturnPlace || written.values.size() != 1) return nullptr;
            return currentReturnPlace;
        }

        /*
         * A field of the record being built - or the box that field is, allocated here rather than
         * after the producer has filled a temporary. That order is the whole of what a later pass
         * cannot recover: `allocateHeap` emitted below the writes does not dominate them.
         */
        case Inst::Aggregate: {
            auto& written = (InstAggregate&)*consumer;
            if(padded || !consumer->results.size()) return nullptr;

            auto result = ssa[consumer->results.get(ssa, 0)];
            if(!manifestOf(*result).inMemory) return nullptr;

            auto record = result->type();
            auto self = SsaPtr<Value>(&value - ssa);

            U32 index = 0;
            for(auto used: written.components.contents(ssa)) {
                if(used.value() == self) break;
                index++;
            }

            if(index >= written.components.size()) return nullptr;

            auto storage = storageFor(block, *result);
            if(!storage) return nullptr;

            if(decl[record]->kind == mir::Type::Array) {
                auto stride = elementStrideNative(layout,
                                                  ((mir::ArrayType*)decl[record])->element);
                return offsetAddress(block, storage, index * stride);
            }

            if(decl[record]->kind != mir::Type::Record) return nullptr;

            auto& declared = *(mir::RecordType*)decl[record];
            auto arm = declared.cons.get(decl, written.constructor);
            auto field = U16(arm.first + index);

            if(mir::indirectField(decl, declared.field(decl, field))) {
                return createBox(block, storage, record, field, result->occurrence.shape,
                                 consumer->source);
            }

            // A co-packed field has no address of its own, so there is nothing to place into - the
            // component is a read-modify-write of a word it shares, which `encodePacked` performs.
            auto held = reprOf(layout, record, result->occurrence.shape).fields.get(decl, field);
            if(held.isPacked()) return nullptr;

            return offsetAddress(block, storage, held.offset);
        }

        /*
         * The storage a `store` is about to copy this value into - **move elision**, and the third
         * destination beside the caller's result pointer and a field of a record being built.
         *
         * `let tree = build(depth, round)` is a call with an address-only result followed by a store
         * into the local's slot, and without this it is two frame slots and the value copied between
         * them - `test/bench/findings.md` §102.3, where it is the whole of `Tree` being 3.3% smaller
         * and 9.2% *slower* than the shipping stage, which places it.
         *
         * Three conditions beyond the four above, and each is about what stands between the producer
         * and the store:
         *
         *  - the value is what the store **writes**, not the address it writes through;
         *  - the store is the instruction **directly after** the producer. The producer now fills
         *    the destination early, so anything in between that read the old contents would read the
         *    new ones and anything that wrote them would have its write overwritten. Adjacency is
         *    the cheap statement of "nothing in between", and it is the shape the elaboration emits:
         *    a call and the store of its result are made together;
         *  - and the producer does not itself **name** the destination. A callee handed that storage
         *    as an argument, or a component read out of it, would see the write land while it is
         *    still reading - so no operand of the producing instruction may be rooted in the
         *    destination's allocation. A destination with no allocation root of this frame is
         *    declined outright, because then there is nothing to separate it from.
         *
         * The destination has to be **lowered already**, which is `mapped` answering: an address
         * computed between the producer and the store is one the producer cannot write through, and
         * declining it is the same dominance question the same-block rule above asks of the consumer.
         *
         * And a **narrow place** is not a destination. A co-packed field has no address of its own,
         * so what a store into one performs is a read-modify-write of the word it shares (`pack.cpp`)
         * - there is nothing for the producer to write through. Both halves of `narrowPlace` are
         * asked here as the pure lookups they are, before it would emit anything.
         */
        case Inst::Store: {
            auto& written = (InstStore&)*consumer;
            auto self = SsaPtr<Value>(&value - ssa);
            if(written.value.value() != self) return nullptr;

            auto& target = *ssa[written.address.value()];
            if(virtualPlaces.getValue(target.id.index)) return nullptr;
            if(narrowPointee(target)) return nullptr;

            if(!directlyFollows(ssa, *ssa[consumer->block], value.definer,
                                value.useAt(ssa, 0))) {
                return nullptr;
            }

            auto root = allocationRoot(ssa, written.address.value());
            if(!root) return nullptr;

            auto shared = false;
            forEachUse(ssa, *ssa[value.definer], [&](Use used) {
                if(used.isSet() && allocationRoot(ssa, used.value()) == root) shared = true;
            });

            if(shared) return nullptr;

            auto place = mapped(written.address.value());
            if(!place) return nullptr;

            /*
             * And a **padded word** is zeroed here rather than declining the placement, which is the
             * one difference from the two destinations above.
             *
             * They decline because their storage is not this rewrite's to prepare: a `return` writes
             * the caller's slot, which the caller zeroed when it made it, and an aggregate component
             * writes into a record whose own storage `storageFor` has already zeroed as a whole. A
             * `store`'s destination is neither - it is a slot that has held other values, so the
             * bits above a co-packed run hold whatever the last one left there. The store this
             * replaces wrote every byte, padding included; zeroing first is what keeps that true,
             * and it is strictly cheaper than the copy, which wrote those bytes *and* read them.
             */
            /*
             * Unless the producer writes every byte, which a **duplicate** does: `Inst::Copy` is a
             * block move of the whole manifestation, padding included, and is exactly what the
             * store it is replacing would have performed. An aggregate is the case the rule is for,
             * because it fills field by field and leaves the bits between them at whatever the slot
             * last held.
             */
            if(padded && ssa[value.definer]->kind != Inst::Copy) {
                zeroStorage(block, place - lower, manifest.size);
            }

            return place - lower;
        }

        default:
            return nullptr;
    }
}

// The argument is in internal.h, beside the declaration. Memoized through `values`, which is where
// every producer's answer already lives - so an aggregate whose storage a component asked for first
// finds it again when its own instruction is reached.
LowerPtr<LowerValue> Lowering::storageFor(LowerBlock& block, Value& value) {
    if(auto found = values.getValue(value.id.index)) return found.unwrap();

    auto destination = destinationFor(block, value);
    auto storage = destination ? destination : frameStorage(block, value.occurrence);

    if(destination) destinations.add(value.id.index, destination);
    values.add(value.id.index, storage);
    return storage;
}

/*
 * A run whose every element is the same byte, as one `setpattern` - the block fill.
 *
 * `[0] :: [U8 *64]` is a fill written as one element, and `resolveArrayLiteral` spreads it into the
 * same sixty-four-component aggregate a hand-written literal produces, precisely so that nothing
 * between there and here needs a second shape to reason about. This is where it collapses again -
 * and it collapses a *hand-written* run of zeroes on the same terms, which is why the recognition is
 * over the components rather than over the syntax that built them.
 *
 * Four things have to hold, and each is read off the values in hand rather than claimed by whoever
 * built them:
 *
 *  - **A constant number.** `setpattern` names a byte, and a value known only at run time cannot be
 *    shown to have a uniform one however wide it is. `constantBits` is what a number is here, which
 *    is the same function the `Const` arm lowers one with - so an enum-layout element answers its
 *    stored value and not its constructor index, in one place rather than two.
 *  - **The same constant in every element**, compared by *value*: the fill pushes one component
 *    `count` times and a written-out run of zeroes resolves each separately, and two constants of
 *    one type holding one number are one byte pattern whichever way they arrived.
 *  - **A register value filling the whole slot.** An element that lives in memory is installed by a
 *    block copy rather than a store, and one whose manifestation is narrower than the stride leaves
 *    padding this would write and the stores do not.
 *  - **Every byte of the element equal.** `0` and `-1` qualify at every width and any `U8` does;
 *    `7 :: U32` does not, because `07 07 07 07` is not seven. Read off the number rather than off
 *    the target's byte order, which does not enter: a value whose bytes are all equal reads the same
 *    either way, and one whose bytes are not is refused here.
 *
 * **How many bytes is not one of them.** That is the same answer `install` gives one line up - a
 * whole-value copy becomes a `LowerInstCopy` at every size - and `expandBlockOperations` walks both
 * kinds against one policy, so a fill below the ceiling is unrolled to the widest transfer the
 * machine has and one above it keeps the string instruction. A second threshold here would be this
 * file holding an opinion about a machine it cannot see.
 *
 * A run of one is still refused, and that is not a size rule: one element is already one store, so
 * there is no run to collapse and the expansion would produce the very same instruction.
 */
bool Lowering::uniformFill(LowerBlock& block, LowerPtr<LowerValue> storage, InstAggregate& written,
                           U32 stride) {
    auto components = written.components.contents(ssa);
    if(components.size() < 2 || !stride) return false;

    const mir::Constant* pattern = nullptr;
    U32 width = 0;

    for(auto used: components) {
        auto& value = *ssa[used.value()];

        auto definer = value.definer;
        if(!definer || ssa[definer]->kind != Inst::Const) return false;

        auto& constant = ((InstConst*)ssa[definer])->constant;
        if(constant.kind != mir::Constant::Int && constant.kind != mir::Constant::Enum) return false;

        // The type as well as the number: the byte count comes from the first element's
        // manifestation, and a component of a different width would be filled at the wrong stride.
        if(pattern && !(constant == *pattern)) return false;

        auto manifest = manifestOf(value);
        if(manifest.dynamic || manifest.inMemory || manifest.size != stride) return false;

        pattern = &constant;
        width = manifest.size;
    }

    auto bits = constantBits(decl, *pattern);
    auto byte = bits & 0xff;

    for(U32 i = 1; i < width; i++) {
        if(((bits >> (i * 8)) & 0xff) != byte) return false;
    }

    block.addInst(lower, new (to.arena) LowerInstSetPattern(
        storage, immediate(block, LowerType::Int64, U64(stride) * components.size()),
        immediate(block, LowerType::Int32, byte)));

    return true;
}

/*
 * The zero fields of a record literal, written as one `setpattern` per run rather than one store
 * each - the record's counterpart of `uniformFill`.
 *
 * `emptyToken()` is fifteen fields and fourteen of them zero, and a lexer's own state record is forty.
 * Written field by field that is a run of scalar stores the backend would have to find again and
 * put back together; written as a fill it is one instruction, which `expandBlockOperations` turns
 * into the widest transfers the machine has and `llvm.memset` is what the other backend's pipeline
 * is written to recognize. It is also one *write* - which is what lets `forwardStoredValues` see
 * that a zeroed slot overwritten whole by a copy was never read, where a run of fifteen stores was
 * fifteen records it had to hold at once.
 *
 * A run is the fields that are zero and adjacent in **offset order**, and the bytes between them:
 *
 *  - **Padding is bridged.** It belongs to this record's storage, and nothing may read it - so
 *    zeroing it is free, and the discriminant and the box flags are written after the fill.
 *  - **A field that is not zero ends a run.** Filling across it would write its bytes twice, and
 *    this is not the place to decide that a second write is cheaper than a second fill.
 *  - **So does a component already built in place** - `destinations`, whose bytes are written
 *    *before* this instruction and which a fill in front of the field walk would erase.
 *  - **A packed field ends one too**, being a read-modify-write of a word rather than a value.
 *
 * A record with a boxed field is left alone. The box's slot is written by `createBox`, and whether
 * it already holds a pointer depends on whether the component was built in place - a question this
 * would have to ask again for a record shape that is rare in a literal.
 *
 * A run of one field is refused, as in `uniformFill`: one field is already one store.
 */
static bool isZeroValue(LowerBase lower, LowerValue* value, U32 bytes) {
    auto inst = value->inst();
    if(inst && inst->kind == LowerInst::Bitcast) inst = lower[((LowerInstUnary*)inst)->from]->inst();
    if(!inst || inst->kind != LowerInst::Imm) return false;

    // A floating immediate holds a `double` whatever its type, so `0.5 :: F32` has no bits in its
    // low four bytes - its whole pattern is what says zero. An integer's bits above the field's width
    // are not written, and only the ones below are asked.
    auto imm = (LowerImm*)inst;
    if(isFloat(imm->result.type)) return imm->i == 0;

    auto bits = imm->i;
    if(bytes < 8) bits &= (U64(1) << (bytes * 8)) - 1;
    return bits == 0;
}

void Lowering::zeroFieldRuns(LowerBlock& block, LowerPtr<LowerValue> storage, InstAggregate& written,
                             mir::RecordType& declared, U16 first, Repr& repr,
                             SmallArray<bool, 32>& filled) {
    struct Span {
        U32 start;
        U32 end;
        bool zero;
        Size component;
    };

    SmallArray<Span, 32> spans;
    U16 index = first;

    for(auto used: written.components.contents(ssa)) {
        auto field = index++;
        auto component = filled.size();
        filled.push(false);

        if(mir::indirectField(decl, declared.field(decl, field))) return;

        auto held = repr.fields.get(decl, field);
        if(held.isPacked()) {
            auto bytes = held.isBytewise() ? held.spanBytes() : held.wordBytes;
            spans.push(Span { held.offset, held.offset + bytes, false, component });
            continue;
        }

        auto& value = *ssa[used.value()];
        auto manifest = manifestOf(value);
        if(manifest.dynamic) return;
        if(!manifest.size) continue;

        auto built = destinations.getValue(value.id.index);
        auto source = built || manifest.inMemory ? nullptr : mapped(used.value());
        auto zero = source && isZeroValue(lower, source, manifest.size);

        spans.push(Span { held.offset, held.offset + manifest.size, zero, component });
    }

    // Offset order, which a record's field order need not be. Few enough for an insertion sort.
    for(Size i = 1; i < spans.size(); i++) {
        for(auto j = i; j > 0 && spans[j - 1].start > spans[j].start; j--) {
            auto swapped = spans[j - 1];
            spans[j - 1] = spans[j];
            spans[j] = swapped;
        }
    }

    for(Size i = 0; i < spans.size(); ) {
        if(!spans[i].zero) { i++; continue; }

        auto end = i + 1;
        while(end < spans.size() && spans[end].zero) end++;

        if(end - i >= 2) {
            auto start = spans[i].start;
            auto bytes = spans[end - 1].end - start;

            block.addInst(lower, new (to.arena) LowerInstSetPattern(
                offsetAddress(block, storage, start), immediate(block, LowerType::Int64, bytes),
                immediate(block, LowerType::Int32, 0)));

            for(auto k = i; k < end; k++) filled[spans[k].component] = true;
        }

        i = end;
    }
}

LowerPtr<LowerValue> Lowering::offsetAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                             U32 offset) {
    if(!offset) return address;

    auto constant = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), LowerType::Int64, U64(offset)));

    auto value = block.addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Pointer, address,
        ((LowerInstSingle*)constant)->created().ptr - lower, LowerInst::Add));

    return ((LowerInstSingle*)value)->created().ptr - lower;
}

// The argument is in internal.h, beside the declaration.
LowerPtr<LowerValue> Lowering::elementAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                              DeclPtr<mir::Type> container,
                                              LowerPtr<LowerValue> index) {
    // A fixed array's storage is its elements, so its own address is the base. A window's element is
    // the `Index` instance's to find, through the view's own fields, and never reaches here.
    assertTrue(decl[container]->kind == mir::Type::Array);
    auto base = address;

    auto stride = elementStrideNative(layout, mir::elementTypeOf(decl, program, container));

    auto scale = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), LowerType::Int64, U64(stride)));

    auto offset = block.addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Int64, index,
        ((LowerInstSingle*)scale)->created().ptr - lower, LowerInst::Mul));

    auto at = block.addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Pointer, base,
        ((LowerInstSingle*)offset)->created().ptr - lower, LowerInst::Add));

    return ((LowerInstSingle*)at)->created().ptr - lower;
}

LowerPtr<LowerValue> Lowering::fieldAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                            DeclPtr<mir::Type> record, U16 field,
                                            DeclPtr<mir::OwnershipSchema> shape) {
    auto slot = offsetAddress(block, address, fieldOffsetNative(layout, record, field, shape));

    /*
     * A **boxed** field's slot holds the address of its value rather than the value, so reaching it
     * is a load - `mir::RecordField::boxed`, and `box.cpp` for the rest of what one costs.
     *
     * Here rather than at each caller, and that is the whole reason this function exists: everything
     * above it asks for "the address of field `i`" and gets the address of the field's *value*, so a
     * read, a write, a borrow and a teardown all cross the indirection without any of them knowing
     * it is there. The two operations that want the slot itself - making the box and releasing it -
     * are in `box.cpp` and compute the offset for themselves.
     */
    if(decl[record]->kind != mir::Type::Record) return slot;

    auto& declared = *(mir::RecordType*)decl[record];
    if(field >= declared.fields.size()) return slot;
    if(!mir::indirectField(decl, declared.field(decl, field))) return slot;

    return boxTarget(block, slot);
}

void Lowering::install(LowerBlock& block, LowerPtr<LowerValue> address, Value& value, bool isVolatile) {
    auto manifest = manifestOf(value);

    // A size of zero is `{}` and there is nothing to install. Not the same test as "no bytes": a
    // dynamic manifestation carries a size of zero that means "ask the descriptor", which is the
    // one case reading `size` directly here would get wrong.
    if(!manifest.dynamic && !manifest.size) return;

    auto source = mapped(&value - ssa);
    if(!source) return;

    if(!manifest.inMemory) {
        block.addInst(lower, new (to.arena) LowerInstStore(address, source - lower, manifest.size, isVolatile));
        return;
    }

    /*
     * A value that lives in storage is installed by moving its bytes.
     *
     * That is what a *relocation* is in this language - `doc/spec/core.md`'s "there is no authored
     * relocation" - so a move and a copy of such a value are the same instruction here and differ in
     * what the ownership analysis said about the source. What this does not do is elide the copy
     * when the source is a temporary that nothing else reads: that is §5.6's destination passing,
     * and it is an optimization over this rather than a second meaning for it.
     */
    block.addInst(lower, new (to.arena) LowerInstCopy(
        address, source - lower, sizeValue(block, value.occurrence)));
}

LowerBlock* Lowering::emitTeardown(LowerBlock& block, LowerPtr<LowerValue> address,
                                  DeclPtr<mir::Type> type, bool runDrop, bool runReclaim,
                                  LocationId source, BoxRelease boxes, LowerFunction* home) {
    auto ownership = mir::ownershipOf(decl, type);

    auto authored = [&](intrinsic::Known klass, mir::TeardownKind kind, bool run) {
        if(!run || kind != mir::TeardownKind::Authored) return;

        auto member = mir::teardownMember(context, program, klass, type, source);
        if(!member) return;

        auto found = functions.getValue(member.offset);
        if(!found) {
            context.diagnostics.error("internal: a teardown was reached before it was lowered"_v,
                                      source);
            return;
        }

        /*
         * The member takes `->value: T`, and `T` is address-only wherever a teardown is placed, so
         * the argument is the address itself. That the two agree is not luck: the declaration walk
         * checked the signature, and this passes exactly what a call to it would pass.
         */
        auto callee = block.addInst(lower, new (to.arena) LowerInstFun(
            decl[member]->name, found.unwrap()));

        // Or the fields of it, where the member's convention takes the value apart.
        SmallArray<LowerPtr<LowerValue>, 8> passed;
        passCarried(block, callShapeOf(member), 0, address, passed);

        auto convention = lower[found.unwrap()]->callType;
        call(lower, to, block, 0, U32(passed.size() + 1), convention, [&](LowerInstCall* built) {
            built->used()[0] = ((LowerInstSingle*)callee)->created().ptr - lower;
            for(Size i = 0; i < passed.size(); i++) built->used()[i + 1] = passed[i];
        });
    };

    // The effect first, then the storage: `Drop` runs at last use and `Reclaim` hands back what the
    // value occupied, and a release that happened first would be running an effect over memory that
    // has already been returned.
    authored(intrinsic::Known::drop, ownership.drop, runDrop);
    authored(intrinsic::Known::reclaim, ownership.reclaim, runReclaim);

    /*
     * The derived halves recurse into the members - and each half separately.
     *
     * An authored instance replaces the derived walk *for its own half*, which is why the two flags
     * travel down rather than one: a record with an authored `Drop` and a member that owns something
     * still has that member's storage handed back, and the author is trusted about "I call nothing
     * else" rather than about the members.
     */
    auto childDrop = runDrop && ownership.drop == mir::TeardownKind::Derived;
    auto childReclaim = runReclaim && ownership.reclaim == mir::TeardownKind::Derived;
    if(!childDrop && !childReclaim) return &block;

    /*
     * A **function value**, whose release is a call to the one function that asks the pair's two
     * questions - D17, and `fun.cpp` is where the representation is written down.
     *
     * Emitted once when *either* half is asked, rather than under one of them - which is sound only
     * because `funTeardown` always runs both: the captures' full `Drop`-and-`Reclaim` walk and an
     * unconditional `freeHeap`, with no `childReclaim ? boxes : BoxRelease::Foreign` downgrade of
     * the kind the ordinary field walk below applies to a boxed field. A record with an authored
     * instance of either half still owes the function value it holds, and running the shared
     * teardown twice is the thing this avoids - but `assertTrue` below is what actually keeps it
     * sound, not an argument that the two halves cannot come apart for a `Fun`.
     *
     * They can: a plain `Fun` field reaches this with the *enclosing* record's own `childDrop` and
     * `childReclaim`, unequal exactly when the record's does, and a `@box (T -> U)` field reaches it
     * the same way through `emitBoxTeardown`'s `target` recursion. A drop-only request should run
     * the captures' effects and skip `freeHeap`; a reclaim-only request should free without running
     * them - which is two more cached bodies (`closureBlockTeardown` keyed on the half pair as well
     * as the environment, and `funTeardown` built once per pair rather than once) for a caller that
     * does not exist today: nothing in `lib/` or `test/` builds a record whose derived `Drop` and
     * `Reclaim` disagree over a `Fun`-typed field, checked by instrumenting this branch and running
     * the whole suite and `test/base` without it ever firing unequal. So this stays the one shared
     * function and the assertion is the fence around the assumption, not a proof of it.
     *
     * A site that can see which lambda this is does not reach here at all: `ownershipOfOccurrence`
     * answers "nothing to release" for a value carrying `Shapes::closure`, so a frame-placed
     * closure's drop is never emitted rather than emitted and folded.
     *
     * An **erased iterator object** is the same two words over the same block, so it is released the
     * same way and by the same function - see `mir::IterType`, where the representation is argued.
     */
    if(decl[type]->kind == mir::Type::Fun || decl[type]->kind == mir::Type::Iter) {
        assertTrue(childDrop == childReclaim);

        auto callee = funTeardown(source);
        auto name = block.addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

        call(lower, to, block, 0, 2, lower[callee]->callType, [&](LowerInstCall* built) {
            built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
            built->used()[1] = address;
        });

        return &block;
    }

    /*
     * A **fixed array** releases each of its elements, unrolled at the stride.
     *
     * Unrolled rather than looped, and that is a decision the bound on the count is what allows:
     * `kMaxFixedArrayLength` keeps this from being a walk no compiler finishes, and the shape a
     * fixed array exists for is a small inline run - `[U8 *64]`, `[U32 *8]` - whose elements almost
     * never owe anything at all. A loop would need control flow, and `emitTeardown` emits into a
     * block its caller is in the middle of; that is the same constraint a sum's teardown is a *call*
     * for, and the day a long array of droppable elements is a real shape, this is the arm that
     * becomes one too.
     *
     * A quantified count cannot reach here: a body compiled once against `[T *n]` releases through
     * the descriptor its caller supplied, and a specialized one has a number.
     */
    if(decl[type]->kind == mir::Type::Array) {
        auto& array = *(mir::ArrayType*)decl[type];
        if(!array.hasFixedLength() || !mir::hasTeardown(decl, array.element)) return &block;

        auto stride = elementStrideNative(layout, array.element);
        auto current = &block;

        for(U64 i = 0; i < array.length; i++) {
            current = emitTeardown(*current, offsetAddress(*current, address, U32(stride * i)),
                                   array.element, childDrop, childReclaim, source, boxes, home);
        }

        return current;
    }

    if(decl[type]->kind != mir::Type::Record) return &block;

    auto record = (mir::RecordType*)decl[type];

    /*
     * A **sum**'s members are alternatives, so which of them to release is a question about the
     * value - and this walk has no control flow to ask it in.
     *
     * So a sum's derived teardown is a **call**, to the per-type function `teardownFor` already
     * builds for a descriptor. That is the same shape the closure release used to have and it is
     * here for the same reason: `emitTeardown` emits into a block its caller is in the middle of,
     * and splitting that block at every drop of a sum would put control flow into a walk that has
     * none. The branch is inside the callee, where there are blocks to put it in.
     *
     * **Unless the caller handed over a function to put blocks in**, and then the constructor test
     * is made here as well - `home`, and it is the whole of J58's answer to `Tree`.
     *
     * The callee begins by deciding the same tag and returning where nothing is owed, so a sum whose
     * value owes nothing costs a call and a `ret` to find that out. That is most of a teardown: a
     * `Maybe(Tree)` at the bottom of the tree is `Nothing` for every leaf's two children, so a
     * thirteen-deep tree made **32,767** calls where the shipping stage's site-side test makes
     * 16,383 - one per node that exists rather than one per edge that might. `test/bench/findings.md`
     * §103.4 is where that was measured as the whole of the program's remaining 9.6%.
     *
     * The guard is one comparison, so it is emitted only where one comparison decides it -
     * `soleOwingConstructor`. What it costs where it fires is a test the callee then repeats, on the
     * path where there was something to do anyway.
     */
    if(record->layout != mir::RecordLayout::Single) {
        auto callee = teardownHalves(type, childDrop, childReclaim, false, source, boxes);
        auto owing = home ? soleOwingConstructor(type) : Maybe<U64>(Nothing());

        auto owed = &block;
        LowerBlock* rest = &block;

        if(owing) {
            auto tagLower = LowerType::Int32;
            auto tag = discriminantOf(block, address, type, tagLower);

            auto number = block.addInst(lower, new (to.arena) LowerImm(
                StringId(), tagLower, owing.unwrap()));

            auto equal = block.addInst(lower, new (to.arena) LowerInstCmp(
                StringId(), tag, ((LowerInstSingle*)number)->created().ptr - lower, LowerCmp::eq));

            owed = home->addBlock(lower, StringId());
            rest = home->addBlock(lower, StringId());

            block.addInst(lower, new (to.arena) LowerInstJe(
                ((LowerInstSingle*)equal)->created().ptr - lower, owed - lower, rest - lower));
        }

        auto name = owed->addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));
        call(lower, to, *owed, 0, 2, lower[callee]->callType, [&](LowerInstCall* built) {
            built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
            built->used()[1] = address;
        });

        if(owed != rest) owed->addInst(lower, new (to.arena) LowerInstJmp(rest - lower));
        return rest;
    }

    auto current = &block;

    for(auto field: record->fields.contents(decl)) {
        // `tearsDownField` and not `hasTeardown` on the type: a field in an origin slot is storage
        // the caller owns, and a record holding one beside an owned guard used to release both.
        if(!mir::tearsDownField(decl, field)) continue;

        /*
         * A **boxed** field is a call rather than an expansion - `emitBoxTeardown`, where the two
         * reasons are written. The one that matters here is that a recursive type reaches its own
         * teardown through exactly this edge, and a walk that expanded it would not terminate.
         *
         * Only the **reclaim** half releases storage, which is `doc/spec/ownership.md`'s split
         * stated one level down: `Drop` is the effect at last use and runs through the pointer
         * exactly as it would have run inline, and handing the allocation back is the other half.
         * So a walk emitting the drop half alone passes `Foreign` and frees nothing.
         */
        if(mir::indirectField(decl, field)) {
            current = emitBoxTeardown(*current, address, type, field.index, nullptr, childDrop,
                                      childReclaim, childReclaim ? boxes : BoxRelease::Foreign,
                                      source, home);
            continue;
        }

        current = emitTeardown(*current, fieldAddress(*current, address, type, field.index),
                               field.occurrence.type, childDrop, childReclaim, source, boxes, home);
    }

    return current;
}

/*
 * Which constructor is in there, however this record spells that - a load of the tag word, or the
 * range test a folded one decodes to. Asked of the layout rather than assumed, on the same terms
 * every other reader of a discriminant asks it.
 *
 * Two readers: `emitSumTeardown`, which branches to one arm per constructor that owes anything, and
 * the guard `emitTeardown` puts in front of a call to it. `tagLower` is answered as well as read,
 * since the width the decode produces is the width its readers have to compare against.
 */
LowerPtr<LowerValue> Lowering::discriminantOf(LowerBlock& block, LowerPtr<LowerValue> address,
                                              DeclPtr<mir::Type> type, LowerType& tagLower) {
    auto& record = *(mir::RecordType*)decl[type];
    auto repr = reprOf(layout, type);

    tagLower = mir::tagBitsOf(decl, record) > 32 ? LowerType::Int64 : LowerType::Int32;

    if(repr.isFolded()) {
        VirtualPlace place;
        place.kind = VirtualPlace::Folded;
        place.word = address;
        place.record = type;

        return decodeFolded(block, place, tagLower);
    }

    if(repr.isBitTagged()) {
        return decodePacked(block, discriminantPlace(block, address, repr), tagLower);
    }

    return wordAt(block, address, repr.discriminantOffset, tagLower, repr.discriminantBytes);
}

/*
 * The one constructor of this sum that owes anything, and nothing where none or several do.
 *
 * What it is for is the guard below: a call whose callee will find nothing to do is worth skipping
 * only if the site can decide that in one comparison, and one owing constructor is exactly when it
 * can. `Maybe(T)` is the shape - `Nothing` owes nothing at all - and it is most of the corpus's
 * sums, since an optional owner is what a recursive type is built out of.
 */
Maybe<U64> Lowering::soleOwingConstructor(DeclPtr<mir::Type> type) {
    if(decl[type]->kind != mir::Type::Record) return Nothing();

    auto& record = *(mir::RecordType*)decl[type];
    if(record.layout == mir::RecordLayout::Single) return Nothing();

    Maybe<U64> owing = Nothing();

    for(auto constructor: record.cons.contents(decl)) {
        auto owes = false;
        for(U16 i = constructor.first; i < U16(constructor.first + constructor.count); i++) {
            if(mir::tearsDownField(decl, record.fields.get(decl, i))) owes = true;
        }

        if(!owes) continue;
        if(owing) return Nothing();

        owing = Just(U64(constructor.value));
    }

    return owing;
}

/*
 * A sum's derived teardown - the one walk in this file that branches.
 *
 * One arm per constructor that owes anything, tested against the discriminant in declaration order,
 * and a fall-through for the ones that owe nothing. Answers the block the caller should carry on in,
 * because that is the whole difference from `emitTeardown`: this one ends somewhere else.
 *
 * A chain of two-way tests rather than a jump table, on the resolver's own reasoning for `match`:
 * every arm here compares one small integer, so the chain and the table run the same comparisons,
 * and the chain needs no instruction this stage does not already emit. What that costs is linear
 * rather than constant time in the number of constructors, on a path that runs once per value death.
 */
LowerBlock* Lowering::emitSumTeardown(LowerFunction& function, LowerBlock& entry,
                                      LowerPtr<LowerValue> address, DeclPtr<mir::Type> type,
                                      bool runDrop, bool runReclaim, LocationId source,
                                      BoxRelease boxes) {
    auto& record = *(mir::RecordType*)decl[type];
    auto tagLower = LowerType::Int32;
    auto tag = discriminantOf(entry, address, type, tagLower);

    auto done = function.addBlock(lower, StringId());
    auto current = &entry;

    for(auto constructor: record.cons.contents(decl)) {
        auto owes = false;
        for(U16 i = constructor.first; i < U16(constructor.first + constructor.count); i++) {
            if(mir::tearsDownField(decl, record.fields.get(decl, i))) owes = true;
        }

        if(!owes) continue;

        auto arm = function.addBlock(lower, StringId());
        auto next = function.addBlock(lower, StringId());

        auto number = current->addInst(lower, new (to.arena) LowerImm(
            StringId(), tagLower, U64(constructor.value)));

        auto equal = current->addInst(lower, new (to.arena) LowerInstCmp(
            StringId(), tag, ((LowerInstSingle*)number)->created().ptr - lower, LowerCmp::eq));

        current->addInst(lower, new (to.arena) LowerInstJe(
            ((LowerInstSingle*)equal)->created().ptr - lower, arm - lower, next - lower));

        for(U16 i = constructor.first; i < U16(constructor.first + constructor.count); i++) {
            auto field = record.fields.get(decl, i);
            if(!mir::tearsDownField(decl, field)) continue;

            if(mir::indirectField(decl, field)) {
                arm = emitBoxTeardown(*arm, address, type, field.index, nullptr, runDrop, runReclaim,
                                      runReclaim ? boxes : BoxRelease::Foreign, source, &function);
                continue;
            }

            arm = emitTeardown(*arm, fieldAddress(*arm, address, type, field.index),
                               field.occurrence.type, runDrop, runReclaim, source, boxes,
                               &function);
        }

        arm->addInst(lower, new (to.arena) LowerInstJmp(done - lower));
        current = next;
    }

    current->addInst(lower, new (to.arena) LowerInstJmp(done - lower));
    return done;
}

/*
 * The two atomic enums, translated into the lower IR's - the one switch each that this seam owes.
 *
 * Written out rather than a cast between two enumerations that happen to agree today, which is the
 * same decision `mir::AtomicOrder`'s own comment makes from the other side: a change to the lower
 * IR's encoding must not silently change what a resolved program means, and a `static_cast` is
 * exactly the arrangement where it would.
 */
static LowerOrder lowerOrder(mir::AtomicOrder order) {
    switch(order) {
        case mir::AtomicOrder::Relaxed:        return LowerOrder::Relaxed;
        case mir::AtomicOrder::Acquire:        return LowerOrder::Acquire;
        case mir::AtomicOrder::Release:        return LowerOrder::Release;
        case mir::AtomicOrder::AcquireRelease: return LowerOrder::AcquireRelease;
        case mir::AtomicOrder::Sequential:     return LowerOrder::Sequential;
    }

    assertTrue(false);
    return LowerOrder::Sequential;
}

// Which arithmetic a read-modify-write performs. Total over `AtomicForm::Update`'s rows and over
// nothing else, which is why the arms outside that group assert rather than answering.
static LowerAtomicOp atomicRmwOp(mir::AtomicOp op) {
    switch(op) {
        case mir::AtomicOp::Exchange: return LowerAtomicOp::Exchange;
        case mir::AtomicOp::Add:      return LowerAtomicOp::Add;
        case mir::AtomicOp::Sub:      return LowerAtomicOp::Sub;
        case mir::AtomicOp::And:      return LowerAtomicOp::And;
        case mir::AtomicOp::Or:       return LowerAtomicOp::Or;
        case mir::AtomicOp::Xor:      return LowerAtomicOp::Xor;
        default: break;
    }

    assertTrue(false);
    return LowerAtomicOp::Add;
}

/*
 * A constant in a register, or the address of one that has none - the whole of what a `const`
 * lowers to, extracted so that a `map` answers with it.
 *
 * A `map` whose entries all agree reads no selector at all and *is* one of these, and the residual
 * table's slots are laid out by the same `materialize` this reaches through `placeConstant` - so a
 * second copy here would be a second answer to "where does a constant of a memory type live".
 */
LowerPtr<LowerValue> Lowering::emitConstant(LowerBlock& block, const mir::Constant& constant,
                                            const mir::Occurrence& occurrence) {
    auto manifest = manifestNative(layout, occurrence);

    /*
     * A constant with **no register form** - a record, which is storage.
     *
     * What a value of a memory type is is the address of its bytes, so a constant of one has to be
     * placed somewhere before it can be named; `placeConstant` is where that decision is, and what
     * comes back here is an address like any other. A `let` of it then copies from that address
     * exactly as it copies from any other record.
     */
    if(manifest.inMemory) {
        auto placed = placeConstant(constant, occurrence);
        auto address = block.addInst(lower, new (to.arena) LowerInstGlobal(
            lower[placed]->name, placed));

        return &((LowerInstSingle*)address)->result - lower;
    }

    /*
     * The address of a global **in a register** - a `lea`, not an immediate.
     *
     * The one constant leaf whose value is not known until the image is placed, which is why
     * `constantBits` asserts on it rather than answering a number: what puts an address in a
     * register is a relocation, and the instruction that carries one is `LowerInstGlobal`. In
     * *storage* the same leaf is `materialize`'s `LowerDataRelocation`; this is the register half of
     * that one decision.
     *
     * A string literal is what reaches it. The value the library builds holds the address of the
     * bytes, and the optimizer answering a read of that field with the constant it found in the slot
     * is a pointer arriving where, before, only a number could.
     */
    if(constant.kind == mir::Constant::Address) {
        auto found = globals.getValue(constant.asGlobal().offset);
        if(!found) {
            context.diagnostics.error("internal: a global was reached before it was lowered"_v,
                                      LocationId());
            return LowerPtr<LowerValue>();
        }

        auto address = block.addInst(lower, new (to.arena) LowerInstGlobal(
            lower[found.unwrap()]->name, found.unwrap()));

        return &((LowerInstSingle*)address)->result - lower;
    }

    if(constant.kind == mir::Constant::Function) {
        auto found = functions.getValue(constant.asFunction().offset);
        if(!found) {
            context.diagnostics.error("internal: a function was reached before it was lowered"_v,
                                      LocationId());
            return LowerPtr<LowerValue>();
        }

        auto address = block.addInst(lower, new (to.arena) LowerInstFun(
            lower[found.unwrap()]->name, found.unwrap()));
        return &((LowerInstSingle*)address)->result - lower;
    }

    LowerInst* value;
    if(constant.kind == mir::Constant::Float) {
        value = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), manifest.type, constant.asFloat()));
    } else {
        /*
         * An **address** is an integer immediate reinterpreted, because the lower IR has no pointer
         * immediate - `validateImm` says so, and the reason it says so is that a target's
         * relocations are what put an address in a register, so a literal one has to arrive as a
         * number. `let nowhere = 0 :: %U8` is the program (`Global.yana`), and the bitcast costs
         * nothing.
         *
         * `constantBits`, not `bits` - an enum constant names a constructor by index and the number
         * it stores is the declaration's. See the note on the declaration.
         */
        auto address = isPtr(manifest.type);
        auto width = address ? (layout.target.pointerSize > 4 ? LowerType::Int64
                                                             : LowerType::Int32)
                             : manifest.type;

        value = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), width, constantBits(decl, constant)));

        if(address) {
            value = block.addInst(lower, new (to.arena) LowerInstUnary(
                LowerInst::Bitcast, StringId(), manifest.type,
                ((LowerInstSingle*)value)->created().ptr - lower));
        }
    }

    return &((LowerInstSingle*)value)->result - lower;
}

void Lowering::lowerInst(LowerBlock& block, Inst& inst) {
    switch(inst.kind) {
        case Inst::Const: {
            auto& written = (InstConst&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];

            values.add(result->id.index,
                       emitConstant(block, written.constant, result->occurrence));
            break;
        }
        case Inst::Map:
            lowerMap(block, (InstMap&)inst);
            break;
        case Inst::GlobalAddress: {
            auto& written = (InstGlobalAddress&)inst;
            auto found = globals.getValue(written.global.offset);
            if(!found) {
                context.diagnostics.error("internal: a global was reached before it was lowered"_v,
                                          inst.source);
                return;
            }

            auto result = ssa[inst.results.get(ssa, 0)];
            auto value = block.addInst(lower, new (to.arena) LowerInstGlobal(
                decl[written.global]->name, found.unwrap()));

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }
        case Inst::Alloc: {
            auto& written = (InstAlloc&)inst;

            // The manifestation of what the slot *holds*, under the occurrence it holds it as -
            // which is why `InstAlloc::held` is an occurrence and not a type.
            auto result = ssa[inst.results.get(ssa, 0)];

            /*
             * The rung, carried down and not decided here - `mir::selectStorage` is the one thing
             * that decides one. `Region` is unreachable (`mir::verifyBody` refuses it), so the two
             * arms are the two shapes an allocation has on this target.
             */
            auto storage = written.storage == mir::StorageClass::Frame
                ? frameStorage(block, written.held)
                : heapStorage(block, written.held, inst.source);

            values.add(result->id.index, storage);
            break;
        }
        /*
         * A run's slots - the request, answered by whichever of the two allocations `place.cpp`
         * decided on.
         *
         * The **same two helpers an `Alloc` reaches**, which is what makes this row cheap: a run of
         * `capacity` elements is a `[a *capacity]`, so the storage question is one this stage
         * already answers and the only thing this arm supplies is the type to ask it about. What
         * separates the two rows is not how the bytes are obtained but who hands them back -
         * `releaseSlots` frees a heap `Alloc` at every exit, and these bytes are released by the
         * container's own `Reclaim` if at all.
         *
         * A count of zero is not representable here and does not arrive: an empty literal is a null
         * pointer and a capacity of nothing, which the resolver emits directly rather than asking
         * for an allocation of no bytes.
         */
        case Inst::RunSlots: {
            auto& written = (InstRunSlots&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];

            auto slots = program.types.arrayType(decl, written.element, written.capacity);
            auto held = mir::owned(slots);

            values.add(result->id.index, written.placed ? frameStorage(block, held)
                                                        : heapStorage(block, held, inst.source));
            break;
        }
        /*
         * And the release, which is one call and no bookkeeping.
         *
         * Unconditional: `InstFree`'s address is an `Alloc`'s own result, and this stage only ever
         * emits one for an allocation it made on the heap. The three-way `BoxRelease` question a
         * boxed *field* has does not arise here - that one exists because a field's release site may
         * be reached through a borrow and cannot see where the caller placed the value, and this
         * site is the frame that allocated.
         */
        case Inst::Free: {
            auto address = mapped(((InstFree&)inst).address.value());
            if(!address) return;

            emitFreeHeap(block, address - lower, inst.source);
            break;
        }
        /*
         * A const parameter, out of the slot the schema numbers for it.
         *
         * The slot is as wide as the type the parameter counts in - `slotBytes` - so the read is of
         * the whole slot and the narrowing to the value's own width is a cast rather than a shorter
         * load. That is what makes it right on a target whose low byte is at the other end, and it
         * is also what stopped an `I64` count from reading four bytes of the slot beside it.
         */
        case Inst::GenValue: {
            auto& written = (InstGenValue&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];

            auto value = numberSlot(block, variableSlot(written.variable), manifestOf(*result).type);
            values.add(result->id.index, value);
            break;
        }
        /*
         * A measurement of a type: an immediate where this compilation laid the type out, and a read
         * of the caller's descriptor where it did not.
         *
         * The second half is the whole reason this is an instruction rather than a constant the
         * conversion folded. `metricValue` is where both live, beside `sizeValue`, because they read
         * the same cells and a second reader of that encoding is a second thing to keep in step.
         */
        case Inst::Metric: {
            auto& written = (InstMetric&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];

            values.add(result->id.index, metricValue(block, written.metric, mir::owned(written.of),
                                                     manifestOf(*result).type));
            break;
        }
        case Inst::FieldAddress: {
            auto& written = (InstFieldAddress&)inst;
            auto address = mapped(written.base.value());
            if(!address) return;

            auto result = ssa[inst.results.get(ssa, 0)];

            /*
             * A structural field requirement, whose position is in a property slot - `erase.cpp`.
             *
             * The same `add` a concrete projection produces, over a number that is loaded rather
             * than written into the instruction, and the load and the store that follow it are the
             * ones the field's *declared type* decides - which the body has, because the requirement
             * wrote it. That is what makes this an `add` rather than a call through an accessor: the
             * only thing a body compiled once is missing about `a.size` is where it is.
             *
             * The slot's two halves - the field's byte offset, and the bit shift above it - are
             * taken apart by `propertyPlace` where the form says to and left alone where it does
             * not. See `kPropertyShiftShift`, where the encoding is argued, and `writePropertySlot`,
             * which is the end that fills them in from the subject's own layout.
             *
             * Specialization spends the requirement instead: `convert.cpp` turns the place into a
             * plain `field.addr` at the substituted subject, so what comes out there is
             * Implementation-Generics.md's "the same direct projection concrete code would have
             * used" - the same instructions a hand-written access compiles to, with no slot in them.
             */
            if(written.property != InstFieldAddress::kNoProperty) {
                auto slot = requirementSlot(mir::GenSlotKind::Property, written.property);

                /*
                 * The field's type as the **requirement** declared it, which is what decides the
                 * shape - and deliberately not the type this instruction's result carries. That one
                 * is `%b`, a *pointer* to the field, because a field address is what the node
                 * produces; asking it for a shape reads `Ptr` where `Bool` was written and puts
                 * every narrow field back on the plain path. `environmentFor` fills the slot from
                 * this same declaration, which is what makes the two ends one decision.
                 */
                auto required = decl[currentGen]->properties.get(decl, written.property);
                auto held = mir::owned(required.result);
                auto form = propertyFormOf(layout, required.result);

                /*
                 * A field that shares a word has no address to hand over, whether the body could
                 * see the record or not - so what a narrow slot produces is the same
                 * `VirtualPlace` a co-packed field of a visible record produces, and the load or
                 * store that follows spends it through `pack.cpp` like any other.
                 */
                if(form == PropertyForm::Narrow) {
                    auto place = propertyPlace(block, address - lower, slot, held);

                    virtualPlaces.add(result->id.index, place);
                    values.add(result->id.index, place.word);
                    break;
                }

                auto place = readProperty(block, address - lower, slot, held, form);
                values.add(result->id.index, place);

                // Only the shape that copied has anything to put back - see `PropertyAccess`.
                if(form == PropertyForm::Witness) {
                    propertyAccesses.add(result->id.index, PropertyAccess {
                        address - lower, place, slot, form, held });
                }

                break;
            }

            /*
             * A component of something that itself has no address - `p.flags.a`, where `flags`
             * shares a word with its neighbours.
             *
             * Ahead of both rows below because it decides them: a scalarized record's fields all sit
             * at offset zero of it, so the shifts **compose** and a field of a packed value is at
             * the sum of the two bit offsets in the same word. Nothing is dereferenced and no
             * temporary is made, which is what keeps a nested packed access the same one load and
             * one shift a flat one is.
             *
             * The **discriminant** of one is the identity: the only records this target packs are
             * enum-layout ones and records it scalarized, and an enum-layout record *is* its
             * discriminant - the same bits at the same shift.
             */
            if(auto outer = narrowPlace(block, *ssa[written.base.value()], address - lower)) {
                auto place = outer.unwrap();
                assertTrue(place.kind == VirtualPlace::Packed);

                auto base = reprOf(layout, written.record, written.shape);

                if(written.field == InstFieldAddress::kDiscriminant) {
                    /*
                     * The tag's own bit range inside the record, which for an enum-layout one is the
                     * whole of it from bit zero and for a bit-tagged sum is the few bits above its
                     * payload. Both are stated on the `Repr`, which is what makes this the same
                     * composition the field row below makes rather than a case of its own.
                     *
                     * Never signed: a record with a tag that reaches below zero fills its storage
                     * and is not one this target packs, so a packed base's tag is always a low run.
                     */
                    place.bitOffset += base.discriminantBitOffset;
                    place.bitWidth = base.discriminantBits;
                    place.isSigned = false;
                } else {
                    auto inner = base.fields.get(decl, written.field);

                    assertTrue(inner.offset == 0); // a scalarized record is one word, fields inside it

                    place.bitOffset += inner.bitOffset;
                    if(inner.bitWidth) place.bitWidth = inner.bitWidth;
                    place.isSigned = manifestNative(layout, inner.occurrence).isSigned;

                    if(!checkAddressable(*result, written.record, written.field, inst.source)) {
                        return;
                    }
                }

                virtualPlaces.add(result->id.index, place);
                values.add(result->id.index, place.word);
                break;
            }

            /*
             * The discriminant - the same `add` over the offset the layout gave it, which is the
             * whole of what `kDiscriminant` means. What is loaded from it is the tag's own integer
             * type, so the load's width comes off the result's occurrence like every other.
             *
             * Unless it was **folded**, in which case there is nothing at any offset to load: the
             * payload's own bits are the tag, and which pattern means which constructor is the
             * encoding's answer. The projection then hands over the payload's address and records
             * what the load or the store that follows has to do with it - `VirtualPlace`.
             */
            if(written.field == InstFieldAddress::kDiscriminant) {
                auto repr = reprOf(layout, written.record, written.shape);

                if(repr.isFolded()) {
                    VirtualPlace place;
                    place.kind = VirtualPlace::Folded;
                    place.word = address - lower;
                    place.record = written.record;
                    place.shape = written.shape;

                    virtualPlaces.add(result->id.index, place);
                    values.add(result->id.index, place.word);
                    break;
                }

                // A tag of bits above the payload is a co-packed field like any other, and the same
                // read-modify-write serves it - which is the whole reason `bitTagSums` asked nothing
                // of this file that `packFields` had not already bought.
                if(repr.isBitTagged()) {
                    auto place = discriminantPlace(block, address - lower, repr);

                    virtualPlaces.add(result->id.index, place);
                    values.add(result->id.index, place.word);
                    break;
                }

                values.add(result->id.index,
                           offsetAddress(block, address - lower, repr.discriminantOffset));
                break;
            }

            /*
             * Where the field is, which the record's own `Repr` answers - offset, and for a
             * co-packed one the word and the bit range inside it.
             *
             * The **subject's** shape travels with the record it names - `InstFieldAddress::shape`.
             * `Maybe('v)`'s payload sits where a reference sits and `Maybe(v)`'s where the value
             * does, and the address alone cannot say which: its own type is a pointer.
             */
            auto held = reprOf(layout, written.record, written.shape).fields.get(decl, written.field);

            if(held.isPacked()) {
                VirtualPlace place;
                place.kind = VirtualPlace::Packed;
                place.word = offsetAddress(block, address - lower, held.offset);
                place.wordBytes = held.wordBytes;
                place.bitOffset = held.bitOffset;
                place.bitWidth = held.bitWidth;
                place.isSigned = manifestNative(layout, held.occurrence).isSigned;

                if(!checkAddressable(*result, written.record, written.field, inst.source)) return;

                virtualPlaces.add(result->id.index, place);
                values.add(result->id.index, place.word);
                break;
            }

            /*
             * Through `fieldAddress` rather than `offsetAddress` on the offset just read, because a
             * **boxed** field's slot holds the address of its value rather than the value: what this
             * instruction produces is the address of the *field*, and for a box that is a load out of
             * the slot rather than the slot.
             *
             * The `Repr` above is still what decided the packed case, and asking `fieldAddress` for
             * the offset again is one table read for a walk that runs once per projection. What it
             * buys is that the indirection is written in exactly one place - this arm and the
             * teardown walk used to compute a field's address two ways, and only one of them would
             * have learned about the box.
             */
            values.add(result->id.index,
                       fieldAddress(block, address - lower, written.record, written.field,
                                    written.shape));
            break;
        }

        // `xs[i]` on a fixed array - the whole of it is `elementAddress`.
        case Inst::ElementAddress: {
            auto& written = (InstElementAddress&)inst;
            auto address = mapped(written.base.value());
            auto index = mapped(written.index.value());
            if(!address || !index) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            values.add(result->id.index,
                       elementAddress(block, address - lower, written.container, index - lower));
            break;
        }

        case Inst::Store: {
            auto& written = (InstStore&)inst;
            auto address = mapped(written.address.value());
            if(!address) return;

            /*
             * Nothing to write where the producer wrote straight into this place - `storageFor`,
             * which is what handed it this address. The recorded destination is compared rather than
             * merely looked up, because a value placed at a destination has exactly one use and this
             * store is it: the two can only differ if that reasoning is wrong somewhere else.
             */
            auto placed = destinations.getValue(ssa[written.value.value()]->id.index);
            if(placed && placed.unwrap() == address - lower) break;

            /*
             * A component with no address of its own - a co-packed field, whose write is a
             * read-modify-write of the word it shares. `pack.cpp` is the pair.
             */
            auto& target = *ssa[written.address.value()];

            if(auto found = narrowPlace(block, target, address - lower)) {
                auto place = found.unwrap();
                auto value = packedBits(block, *ssa[written.value.value()]);
                if(!value) return;

                /*
                 * A folded tag is only ever written by the construction that decides which
                 * constructor is being built, which is `Inst::Aggregate` and is where the pattern is
                 * a literal. A store to one would be a *computed* constructor, which the language
                 * has no way to write and this encoding has no way to serve.
                 */
                if(place.kind == VirtualPlace::Folded) {
                    context.diagnostics.error(
                        "internal: a folded discriminant was written outside a construction"_v,
                        inst.source);
                    return;
                }

                encodePacked(block, place, value);
                break;
            }

            install(block, address - lower, *ssa[written.value.value()], written.isVolatile);

            // A write through a field requirement that was *copied* out lands in the temporary, and
            // the commit is what makes it the subject's - see `readProperty`.
            if(auto access = propertyAccesses.getValue(ssa[written.address.value()]->id.index)) {
                commitProperty(block, access.unwrap());
            }

            break;
        }
        case Inst::Binary: {
            auto& written = (InstBinary&)inst;
            auto lhs = mapped(written.lhs.value());
            auto rhs = mapped(written.rhs.value());
            if(!lhs || !rhs) return;

            auto operandType = ssa[written.lhs.value()]->type();
            auto result = ssa[inst.results.get(ssa, 0)];
            auto kind = binaryKind(decl, written.op, operandType);

            if(kind == LowerInst::Cmp) {
                /*
                 * A comparison needs nothing of the width seam, and that is a *consequence* rather
                 * than an omission: both registers hold the value in normal form, so the wide
                 * comparison and the narrow one are the same question - see `width.cpp`,
                 * where the invariant this rests on is stated.
                 */
                /*
                 * The result type is stated, because a comparison of two **vectors** answers a mask
                 * of their shape where one of two scalars answers a `Bool` - which is the whole of
                 * what a vector comparison changes about this instruction. `binaryResult` is where
                 * the tier above says the same thing, so this is that answer carried down rather
                 * than a second one.
                 */
                auto compare = block.addInst(lower, new (to.arena) LowerInstCmp(
                    StringId(), lhs - lower, rhs - lower,
                    lowerCmp(decl, written.op, operandType), manifestOf(*result).type));

                values.add(result->id.index, &((LowerInstSingle*)compare)->result - lower);
                break;
            }

            auto manifest = manifestOf(*result);

            /*
             * The two saturating rows, which leave here for `Cmp`'s reason and one more: they are
             * not `LowerInstBinary`s (see `LowerInstSaturating`), and they have nothing to do with
             * the width seam below - a lane is a whole lane, and the answer is inside its range by
             * construction, which is `WidthEffect::Closed` in op.def.
             */
            if(kind == LowerInst::SatAdd || kind == LowerInst::ISatAdd
               || kind == LowerInst::SatSub || kind == LowerInst::ISatSub) {
                auto saturating = block.addInst(lower, new (to.arena) LowerInstSaturating(
                    kind, StringId(), manifest.type, lhs - lower, rhs - lower));

                values.add(result->id.index, ((LowerInstSingle*)saturating)->created().ptr - lower);
                break;
            }

            /*
             * The width seam - `width.cpp`, and `op.def`'s width column, which is what says
             * whether this operation has anything to do with it.
             *
             * The *operand's* type and not the result's, and for a shift they are the same one: a
             * shift answers its left operand's type and everything else here answers both operands'.
             * A comparison is the one binary node where they differ, and it left above.
             */
            auto width = narrowWidthOf(operandType);
            auto effect = mir::binaryWidthEffect(written.op);

            if(width && effect == mir::WidthEffect::Rotates) {
                auto rotated = narrowRotate(block, lhs - lower, rhs - lower, width,
                                            written.op == mir::BinaryOp::Rol, StringId());
                values.add(result->id.index, rotated);
                break;
            }

            // `shr` reads the storage of its left operand rather than its value, so the operand is
            // put in range before the shift and not only after it - see `zeroExtended`.
            auto left = width && effect == mir::WidthEffect::Logical
                      ? lower[zeroExtended(block, lhs - lower, width)] : lhs;

            auto value = block.addInst(lower, new (to.arena) LowerInstBinary(
                StringId(), manifest.type, left - lower, rhs - lower, kind));

            auto produced = ((LowerInstSingle*)value)->created().ptr - lower;
            if(width && wrapsAtDeclaredWidth(effect)) {
                produced = putInRange(block, produced, width, StringId());
            }

            values.add(result->id.index, produced);
            break;
        }
        case Inst::Unary: {
            auto& written = (InstUnary&)inst;
            auto operand = mapped(written.value.value());
            if(!operand) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = manifestOf(*result);

            auto operandType = ssa[written.value.value()]->type();
            auto& logical = *decl[operandType];

            /*
             * `represent` - the same value under another occurrence, and no instruction. A view
             * record and the view are one address-only value laid out one way, so the operand's
             * address is the answer; a pointer restated at a result that names its origin is the
             * same register. Both ends manifest alike by construction - `Builder::addRepresent`
             * checks the record pairing and the graft restates only across a return.
             */
            if(written.op == mir::UnaryOp::Represent) {
                values.add(result->id.index, operand - lower);
                break;
            }

            /*
             * A **conversion to the type it already has** is the identity, and no instruction.
             *
             * `foldFunctionConstants` removes it and nothing else did, so the shape only survives to
             * here under `-no-ir-opt` - which is exactly where a rule that costs nothing has to hold:
             * `x64`'s `selectFormForTarget` asserts `from != to` on a float cast, because there is no
             * `cvtsd2sd`. What produces one is a `provided.def` instance at its own type -
             * `F64.fromDecimal(value: F64) -> F64` is `cast value`, and so is `Bitcast(a, a)` - which
             * is a real declaration rather than a mistake: the ladder has a rung between every pair
             * and the diagonal is one of the pairs.
             *
             * The *logical* types, not the manifestations, which is what keeps a `@bits` refinement
             * out: `IntType::canonical` makes a refinement a distinct type precisely so that the
             * narrowing at the store is not mistaken for a no-op, and the two manifest alike.
             */
            if((written.op == mir::UnaryOp::Convert || written.op == mir::UnaryOp::Bitcast) &&
               operandType == result->type()) {
                values.add(result->id.index, operand - lower);
                break;
            }

            LowerInst* value;
            if(written.op == mir::UnaryOp::Not && logical.kind == mir::Type::Record) {
                /*
                 * `!b` at a `Bool` is one exclusive or, not a complement.
                 *
                 * A `Bool` is one byte holding 0 or 1 in a 32-bit register, so inverting every bit
                 * of it answers 254 rather than False. That the two are different is exactly why
                 * `Bool` is not a one-bit integer up above: the operation is logical, and this is
                 * the file that knows what a `Bool` looks like. A record here *is* the `Bool` -
                 * `Bitwise` has no other instance over one, and `op.cpp` admits no other record.
                 */
                auto one = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), manifest.type, U64(1)));

                value = block.addInst(lower, new (to.arena) LowerInstBinary(
                    StringId(), manifest.type, operand - lower,
                    ((LowerInstSingle*)one)->created().ptr - lower, LowerInst::Xor));
            } else if(written.op == mir::UnaryOp::Not) {
                value = block.addInst(lower, new (to.arena) LowerInstUnary(
                    LowerInst::Not, StringId(), manifest.type, operand - lower));
            } else if(written.op == mir::UnaryOp::Convert) {
                /*
                 * `convert` - `cast`, with the two signedness flags the lower IR reads.
                 *
                 * The flags say how a *value* crosses: a signed integer widening sign-extends and an
                 * unsigned one zero-extends, a signed integer becomes a float through its sign and
                 * an unsigned one does not, and a float becomes a signed or an unsigned integer. So
                 * each side's flag is its own logical type's, and a float's is false - it has no
                 * sign bit for the instruction to extend. Narrowing reads neither flag; `cast`
                 * truncates whatever it was given.
                 *
                 * A vector's flags are its lane's: `Vec(I8)` to `Vec(I16)` sign-extends every lane,
                 * as JavaScript converting one lane at a time does.
                 */
                auto laneOf = [&](const mir::Type& type) -> const mir::Type& {
                    return type.kind == mir::Type::Vector ? *decl[((mir::VectorType&)type).lane] : type;
                };

                auto& source = laneOf(logical);
                auto& target = laneOf(*decl[result->type()]);
                auto signedSource = source.kind == mir::Type::Int && ((mir::IntType&)source).isSigned;
                auto signedResult = target.kind == mir::Type::Int && ((mir::IntType&)target).isSigned;

                value = block.addInst(lower, new (to.arena) LowerInstCast(
                    StringId(), manifest.type, operand - lower, signedSource, signedResult));
            } else if(written.op == mir::UnaryOp::Bitcast) {
                /*
                 * `bitcast` - the same bits at another type, and no signedness anywhere.
                 *
                 * A different instruction from the cast above rather than the same one with the
                 * flags off, and the difference is what each *means*: a cast crossing between a
                 * float and an integer converts the number, and this reads the bytes again. Both
                 * ends are the same width by construction - no rung relating a pair that is not was
                 * ever generated, `intrinsic::reinterpretable` - so there is no truncation in the
                 * instruction and no conversion for a sign to decide.
                 *
                 * What the *register* holds above the value is a different question and is settled
                 * below - see the note there. `BitcastNarrow.yana` is the fixture, and the shipping
                 * stage's arm carries the same correction with the wrong answers it produced.
                 */
                value = block.addInst(lower, new (to.arena) LowerInstUnary(
                    LowerInst::Bitcast, StringId(), manifest.type, operand - lower));
            } else if(written.op == mir::UnaryOp::ByteSwap) {
                /*
                 * The byte reversal, and the one width that does not survive this seam.
                 *
                 * At thirty-two and sixty-four bits it is a unary instruction on both sides, exactly
                 * as `Abs` is: every backend has the operation and what it does with one is its own
                 * business. At sixteen it cannot be, because the lower IR has no sixteen-bit scalar
                 * - so this is where the swap is spent, in the terms `width.cpp` already has for a
                 * narrow type. `narrowByteSwap` is that expansion, beside `narrowRotate`, which is
                 * here for the same reason at a different operation.
                 *
                 * The `else` chain below wraps the result at the declared width and this arm does
                 * not need it: `op.def` says the reversal is `Closed`, and the narrow path has
                 * already been through `putInRange` on its way out.
                 */
                if(auto narrow = narrowWidthOf(operandType)) {
                    values.add(result->id.index,
                               narrowByteSwap(block, operand - lower, narrow, StringId()));
                    break;
                }

                value = block.addInst(lower, new (to.arena) LowerInstUnary(
                    LowerInst::Bswap, StringId(), manifest.type, operand - lower));
            } else if(written.op == mir::UnaryOp::CountBits ||
                      written.op == mir::UnaryOp::LeadingZeros ||
                      written.op == mir::UnaryOp::TrailingZeros) {
                /*
                 * The three bit counts, which are lower **intrinsics** rather than unary kinds -
                 * `Popcnt`, `ClzWidth` and `CttzWidth`.
                 *
                 * Both zero counts lower to the *defined* form, because the language's answer at
                 * zero is the type's width and those are the kinds that say so. Whether a machine
                 * has an instruction for that or has to build it out of a bit scan and a
                 * conditional move is the target's question - `expandBitScans` in the x64 backend
                 * is where it is answered, and the LLVM backend hands the same fact to `llvm.ctlz`
                 * as an argument.
                 *
                 * Built by hand rather than through the arm below, for the reason the x64 builder's
                 * `intrinsic` gives: a `LowerInstIntrinsic`'s results live *past* the instruction
                 * rather than inside it, because an intrinsic may answer none or several - so the
                 * allocation is the instruction plus its one result and its one operand.
                 *
                 * No narrowing on the way out and none needed: the answer is between 0 and 64, so
                 * it is in range of every type these are asked at, which is `op.def`'s `Closed`.
                 */
                auto which = written.op == mir::UnaryOp::CountBits    ? LowerIntrinsic::Popcnt
                           : written.op == mir::UnaryOp::LeadingZeros ? LowerIntrinsic::ClzWidth
                                                                      : LowerIntrinsic::CttzWidth;

                auto built = new (allocTrailing<LowerInstIntrinsic, LowerValue, LowerPtr<LowerValue>>(
                    to.arena, sizeof(LowerValue) + sizeof(LowerPtr<LowerValue>))) LowerInstIntrinsic(which, 1, 1);
                built->used().ptr[0] = operand - lower;
                new (built->created().ptr) LowerValue(built, manifest.type, StringId());

                value = block.addInst(lower, (LowerInst*)built);
            } else {
                /*
                 * The rest are one lower instruction each, and the translation is the one switch
                 * this seam owes - written out rather than a cast between two enumerations that
                 * happen to agree, for the reason the atomic orders' translation is.
                 */
                auto kind = LowerInst::Neg;
                switch(written.op) {
                    case mir::UnaryOp::Neg:   kind = LowerInst::Neg;   break;
                    case mir::UnaryOp::Abs:   kind = LowerInst::Abs;   break;
                    case mir::UnaryOp::Sqrt:  kind = LowerInst::Sqrt;  break;
                    case mir::UnaryOp::Trunc: kind = LowerInst::Trunc; break;
                    case mir::UnaryOp::Floor: kind = LowerInst::Floor; break;
                    case mir::UnaryOp::Ceil:  kind = LowerInst::Ceil;  break;
                    case mir::UnaryOp::Round: kind = LowerInst::Round; break;

                    // The four the branches above answered, and any row added without deciding.
                    // Asserted rather than defaulted, because the default would be a negation.
                    default: assertTrue(false); break;
                }

                value = block.addInst(lower, new (to.arena) LowerInstUnary(
                    kind, StringId(), manifest.type, operand - lower));
            }

            /*
             * And back into the declared width where the operation may have left it - the seam,
             * `width.cpp`, read through `op.def`'s width column exactly as the binary arm
             * reads it.
             *
             * The **result's** type here and not the operand's, which is the one thing that differs
             * from a binary node: `convert` is the whole business of crossing between two widths,
             * so what has to hold is what the destination declares. `truncate(w) :: U8` from a `U64`
             * is defined as the low eight bits, and a `cast` alone leaves the low thirty-two.
             *
             * `not(0 :: U8)` is the other half and is why `Not` is in the wrapping set beside the
             * arithmetic: the complement of an in-range value sets every bit above the width, so a
             * register holding it answers 4294967295 where the type answers 255.
             *
             * A `Bool`'s `not` reached the exclusive-or arm above and is already in normal form -
             * `narrowWidthOf` answers nothing for a record, which is what makes that true by
             * construction rather than by the arm being written first.
             */
            auto width = narrowWidthOf(result->type());
            auto produced = ((LowerInstSingle*)value)->created().ptr - lower;

            /*
             * `bitcast` beside the column's answer, and it is the one operation where the two
             * questions come apart.
             *
             * `op.def`'s width column asks whether the arithmetic can leave the type's range, which
             * is true on every machine and is why it lives one tier up. A bitcast's result is in
             * range by construction - it is the same bits under a type of the same width - so
             * `Closed` is the right answer there. What changes is which extension the *register* is
             * required to carry, and a bitcast between the two readings of one narrow width is
             * precisely the point where that flips: `bitcast(-49 :: I8) :: U8` arrives
             * sign-extended and a `U8` is held zero-extended.
             *
             * That was silently repaired by a round trip through the local a fixture writes it to -
             * a store discards the register above the byte - so it stayed invisible until this
             * stage started promoting those slots away and stating the declared range as a hint.
             * `narrowWidthOf` answers nothing at the widths that fill their register, so the pairs
             * that cannot disagree cost nothing.
             */
            auto wraps = wrapsAtDeclaredWidth(mir::unaryWidthEffect(written.op)) ||
                         written.op == mir::UnaryOp::Bitcast;

            if(width && wraps) {
                produced = putInRange(block, produced, width, StringId());
            }

            values.add(result->id.index, produced);
            break;
        }
        case Inst::Load: {
            auto& written = (InstLoad&)inst;
            auto address = mapped(written.address.value());
            if(!address) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = manifestOf(*result);

            /*
             * A component with no address of its own - a co-packed field, or a discriminant folded
             * into a pattern its payload cannot produce.
             *
             * Ahead of everything below because it answers the same question the rows below answer
             * for an ordinary place, and because the first of them - "a value that lives in storage
             * is its address" - is exactly what is not true here. `pack.cpp` holds the four
             * operations and `decodeIntoStorage` is the one row that does need a place to put the
             * answer, since a scalarized record has no register form.
             */
            if(auto found = virtualPlaces.getValue(ssa[written.address.value()]->id.index)) {
                auto place = found.unwrap();

                spendPlace(block, *result, place, inst.source);
                break;
            }

            /*
             * ...and a place reached **through** one: a reference whose pointee is narrow carries
             * the shift alongside the address, so what it names is the same kind of component and
             * the same four operations serve it - `unpackNarrowRef`, which is where the encoding is.
             *
             * Ahead of the plain rows below for the same reason the row above is: what the address
             * names is bits inside a word rather than a value at an address, and every one of those
             * rows would read the word.
             */
            auto& from = *ssa[written.address.value()];
            auto pointee = narrowPointee(from);

            if(pointee && !isReferenceValue(layout, result->occurrence)) {
                spendPlace(block, *result, unpackNarrowRef(block, address - lower, pointee),
                           inst.source);
                break;
            }

            /*
             * A value that lives in storage *is* its address, so a load of one is no instruction.
             *
             * The object stays where it is and what the load produced is the right to reach it - as
             * an owner, as a loan or as the thing a move relocates. Emitting a machine load here
             * would be reading the first eight bytes of a record into a register.
             *
             * A **loan** is the same statement for a different reason and is why the second half is
             * here: the value is the address of the storage the load named, so there is nothing to
             * read. The one place that is not true is a record field that *holds* a reference, and
             * the conversion gives that load an owned pointer occurrence precisely so that it falls
             * through to the real load below - see `addressOf`'s `Deref`.
             *
             * `isReferenceValue` and not "the contract is not owned", which is what this asked and
             * is a *coarser* question than the one the manifestation layer answers. A shared,
             * call-bounded loan of a trivially copyable scalar is a **register value** - `manifest.h`
             * says so by name, because the spec copies at that position - so `x: I32` borrowed had
             * its load skipped and the caller passed an address where the callee's own signature
             * takes four bytes. It only surfaced through a *generic* body, which is where a loan of a
             * scalar is written in the first place: a concrete signature spells `x: I32` and gets a
             * copy, while `x: a` at `a = I32` is the same position reached through a variable.
             */
            if(manifest.inMemory || isReferenceValue(layout, result->occurrence)) {
                values.add(result->id.index, address - lower);
                break;
            }

            // A type with no bytes has nothing to load, and `makeMemoryFlags` has no encoding for a
            // width of zero. Nothing produces one - a `{}` value is not read out of storage - so
            // this is the assertion that says so rather than a case to handle.
            assertTrue(manifest.size != 0);

            /*
             * The access width is the declared one and the register is the manifestation's: a narrow
             * scalar arrives in a 32-bit register and states its width here, which is the rule
             * `LowerLane` is written against.
             *
             * In **bytes**, which is what `makeMemoryFlags` encodes and `getWidth` reads back. A
             * load that stated bits validated anyway for every integer - the flags hold a log2 and
             * 32 is as much a power of two as 4 - and was caught by `F64`, whose validation compares
             * the width against the lane.
             *
             * Whether it sign-extends is the manifestation's answer too - `Manifestation::isSigned`,
             * where the enum-layout record that made it one is argued.
             */
            auto value = block.addInst(lower, new (to.arena) LowerInstLoad(
                address - lower, StringId(), manifest.type, manifest.size, manifest.isSigned,
                written.isVolatile));

            // The tail-read guarantee, spent - `LowerInstLoad::isOverread`, where the three passes
            // that must not read "past the end" as a bug are named. Set here rather than in the
            // constructor because it is a property of the one access `vectorPast` makes, not of the
            // shape of a load.
            if(written.overread) ((LowerInstLoad*)value)->setOverread();

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }
        case Inst::Copy: {
            auto& written = (InstCopy&)inst;
            auto address = mapped(written.address.value());
            if(!address) return;

            /*
             * Fresh storage, with the bytes moved into it.
             *
             * The conversion emits this only where duplication is an operation - a value carried in
             * a register is duplicated by being read - so the address-only case is the only one
             * there is, and the allocation is what makes the second owner a second object.
             */
            auto result = ssa[inst.results.get(ssa, 0)];

            // A co-packed source has no bytes to copy from: the second owner is the decoded value,
            // put in storage of its own - which is what `decodeIntoStorage` already builds, and the
            // whole of what a duplicate of a trivially copyable narrow value is.
            if(auto found = narrowPlace(block, *ssa[written.address.value()], address - lower)) {
                values.add(result->id.index,
                           decodeIntoStorage(block, found.unwrap(), result->occurrence));
                break;
            }

            // A duplicate whose one reader takes it apart into registers is never made at all - see
            // `carriedDuplicate`, where the window it has to be read in is argued.
            if(carriedDuplicate(inst, *result)) {
                values.add(result->id.index, address - lower);
                break;
            }

            /*
             * `storageFor` rather than `frameStorage`, which is the whole of J65's copy elision.
             *
             * A duplicate is a producer of a memory value like any other, and the destination it is
             * going to be copied into is a place it can be built in directly - so `let token =
             * step(..)` is one block copy into the local rather than a temporary, a copy into it and
             * a copy out of it. The four conditions in `destinationFor` are what makes that sound
             * here as everywhere else, and the one that matters for a duplicate is `shared`: a copy
             * whose *source* is rooted in the destination's allocation is a copy onto itself, and it
             * is declined there rather than tested again here.
             *
             * It was `frameStorage` only because the two consumers this began with - a `return` and
             * an aggregate component - are not the ones a duplicate meets. The `store` destination
             * is, and it is the one the driver loop of `test/bench/lexer` spends a third of itself
             * on.
             */
            auto storage = storageFor(block, *result);

            block.addInst(lower, new (to.arena) LowerInstCopy(
                storage, address - lower, sizeValue(block, result->occurrence)));

            break;
        }
        case Inst::Aggregate: {
            auto& written = (InstAggregate&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];

            auto record = result->type();

            /*
             * An **enum-layout** record is its discriminant, so building one is the number and no
             * storage at all - `manifestNative`, which is where that is decided.
             *
             * The manifestation and not the layout is what is asked, for the reason every other
             * consumer asks it: what matters here is that the result has a register form, and a
             * second reader of `RecordLayout` would be a second place that has to agree about which
             * records do.
             */
            auto manifest = manifestOf(*result);
            if(!manifest.inMemory) {
                auto& declared = *(mir::RecordType*)decl[record];
                auto value = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), manifest.type,
                    U64(declared.cons.get(decl, written.constructor).value)));

                values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                break;
            }

            auto storage = storageFor(block, *result);

            /*
             * A **fixed array** - the same walk at a stride instead of at the field offsets.
             *
             * One arm rather than a second `case`, because everything else about building one is
             * identical: the storage, the per-component `install`, and the address the result takes.
             * What differs is where component `i` goes, which for a record is
             * `fieldOffsetNative` and for an array is `i` times the element's stride.
             */
            if(decl[record]->kind == mir::Type::Array) {
                auto stride = elementStrideNative(layout, ((mir::ArrayType*)decl[record])->element);

                if(uniformFill(block, storage, written, stride)) break;

                U32 offset = 0;
                for(auto used: written.components.contents(ssa)) {
                    if(!destinations.getValue(ssa[used.value()]->id.index)) {
                        install(block, offsetAddress(block, storage, offset),
                                *ssa[used.value()]);
                    }

                    offset += stride;
                }

                break;
            }

            auto& declared = *(mir::RecordType*)decl[record];
            auto arm = declared.cons.get(decl, written.constructor);
            auto repr = reprOf(layout, record, result->occurrence.shape);

            // The zero fields first, as fills - see `zeroFieldRuns`. In front of the tag, which a run
            // may bridge and which is written below it.
            SmallArray<bool, 32> filled;
            zeroFieldRuns(block, storage, written, declared, arm.first, repr, filled);

            /*
             * The discriminant first, where the record stores one.
             *
             * A plain store of a literal, because which constructor is being built is known here:
             * this is the only thing that writes a tag, and everything that reads one reads it back
             * the same way. `layoutRecord` is where the placement is decided and says why it is
             * zero.
             *
             * A **folded** tag is the same statement with the store taken out: the constructor that
             * owns the payload is recognized by the payload being there, so building one writes
             * nothing at all, and every other constructor is one impossible pattern.
             */
            switch(repr.discriminant) {
                case Discriminant::None:
                    break;

                case Discriminant::Niche:
                    encodeFolded(block, storage, record, result->occurrence.shape,
                                 written.constructor);
                    break;

                // Bits above the payload, so the same read-modify-write a co-packed field's write
                // is - and the storage this merges into was zeroed, which is what `hasPaddedWord`
                // asks for on this record's behalf.
                case Discriminant::Bits:
                    encodePacked(block, discriminantPlace(block, storage, repr),
                                 immediate(block, LowerType::Int64, U64(arm.value)));
                    break;

                case Discriminant::Word: {
                    auto tag = block.addInst(lower, new (to.arena) LowerImm(
                        StringId(), packLane(repr.discriminantBytes), U64(arm.value)));

                    block.addInst(lower, new (to.arena) LowerInstStore(
                        offsetAddress(block, storage, repr.discriminantOffset),
                        ((LowerInstSingle*)tag)->created().ptr - lower, repr.discriminantBytes));
                    break;
                }
            }

            // One field at a time, into the storage the record occupies. `install` is what decides
            // whether a component is a store or a block copy, which is the same decision every other
            // write into storage makes; a co-packed one is neither, and is the read-modify-write
            // `pack.cpp` holds. The components are this constructor's fields, in its order,
            // and the record's own `Repr` is what turns each index into a position.
            U16 index = arm.first;

            for(auto used: written.components.contents(ssa)) {
                auto position = Size(index - arm.first);
                auto field = index++;
                auto held = repr.fields.get(decl, field);

                /*
                 * A **boxed** component: the storage is made here, and the value goes into it -
                 * `createBox`, which allocates, fills the slot and records the provenance.
                 *
                 * This is the one place a box comes into existence for a whole-record construction,
                 * and there is exactly one other (`Node::Write` at an `Init` into a boxed field, in
                 * `convert.cpp`). An **assignment** is deliberately not one of them: overwriting a
                 * live boxed field writes through the box it already has, which is what keeps the
                 * field's address stable while its value changes and is most of the reason to write
                 * `@box` in the first place.
                 */
                if(mir::indirectField(decl, declared.field(decl, field))) {
                    // Already made, where the component was placed straight into it - `storageFor`,
                    // which allocated the box before the producer wrote so that there is nothing to
                    // copy. A second `createBox` here would leak the first.
                    if(destinations.getValue(ssa[used.value()]->id.index)) continue;

                    auto target = createBox(block, storage, record, field,
                                            result->occurrence.shape, inst.source);
                    if(!target) return;

                    install(block, target, *ssa[used.value()]);
                    continue;
                }

                if(held.isPacked()) {
                    VirtualPlace place;
                    place.kind = VirtualPlace::Packed;
                    place.word = offsetAddress(block, storage, held.offset);
                    place.wordBytes = held.wordBytes;
                    place.bitOffset = held.bitOffset;
                    place.bitWidth = held.bitWidth;

                    auto bits = packedBits(block, *ssa[used.value()]);
                    if(!bits) return;

                    encodePacked(block, place, bits);
                    continue;
                }

                if(destinations.getValue(ssa[used.value()]->id.index)) continue;
                if(position < filled.size() && filled[position]) continue;
                install(block, offsetAddress(block, storage, held.offset), *ssa[used.value()]);
            }

            break;
        }
        case Inst::Drop: {
            auto& written = (InstDrop&)inst;
            auto address = mapped(written.address.value());
            if(!address) return;

            /*
             * A type the body does not know, which is one indirect call through its descriptor.
             *
             * Beside the walk rather than a case inside it, because the two are different kinds of
             * answer: `emitTeardown` *is* the expansion - the members, the authored instances - and
             * this is a call to somebody else's copy of it. What makes them the same teardown is
             * that the function on the other end of the pointer was built by running that walk.
             */
            if(decl[written.held.type]->kind == mir::Type::Variable) {
                emitErasedTeardown(block, address - lower, written.held.type);
                break;
            }

            /*
             * What this site knows about the value's interior boxes - `InstDrop::boxesVisible`, and
             * `BoxRelease` for the three answers.
             *
             * A drop of storage this frame owns frees them outright; one reached through a borrow
             * has to ask, because the caller may have placed the value somewhere this program does
             * not hand back to. Which of the two ways it asks is the build's, and `storesBoxProvenance`
             * decides it - a stored bit, or a question put to the allocator about the address.
             */
            emitTeardown(block, address - lower, written.held.type, true, true, inst.source,
                         written.boxesVisible ? BoxRelease::Owned : BoxRelease::Unknown);
            break;
        }
        case Inst::FunValue: {
            auto& written = (InstFunValue&)inst;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto raw = decl[result->occurrence.type]->kind == mir::Type::Fun &&
                       ((mir::FunType*)decl[result->occurrence.type])->convention;
            if(raw) {
                auto found = functions.getValue(written.callee.offset);
                if(!found) {
                    context.diagnostics.error("internal: a raw function pointer names a function that was not lowered"_v,
                                              inst.source);
                    return;
                }

                auto value = block.addInst(lower, new (to.arena) LowerInstFun(
                    decl[written.callee]->name, found.unwrap()));
                values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                break;
            }

            LowerPtr<LowerValue> environment = nullptr;
            if(written.capturing) {
                auto held = mapped(written.environment.value());
                if(!held) return;
                environment = held - lower;
            }

            auto value = emitFunValue(block, written.callee, environment, inst.source);
            if(!value) return;

            values.add(ssa[inst.results.get(ssa, 0)]->id.index, value);
            break;
        }
        /*
         * An operation the machine has and this IR does not - the shipping stage's `Value::Native`,
         * with the same three shapes and the same argument orders.
         *
         * Nothing about *what* these mean is decided here: the two block forms are instructions the
         * lower IR already has, and a system call is a call with its own convention, which is what
         * makes the number operand zero rather than a field. So this arm is a mapping and the
         * backend is where any of it becomes machine code.
         */
        case Inst::Native: {
            auto& written = (InstNative&)inst;

            SmallArray<LowerPtr<LowerValue>, 8> args;
            for(auto used: written.arguments.contents(ssa)) {
                auto value = mapped(used.value());
                if(!value) return;
                args.push(value - lower);
            }

            auto lowerX86 = [&](mir::NativeOp op) -> LowerInst* {
                auto addIntrinsic = [&](LowerIntrinsic which, Size created, Size used,
                                        bool sourceResult) {
                    auto built = new (allocTrailing<LowerInstIntrinsic, LowerValue, LowerPtr<LowerValue>>(
                        to.arena, sizeof(LowerValue) * created + sizeof(LowerPtr<LowerValue>) * used))
                        LowerInstIntrinsic(which, created, used);
                    for(Size i = 0; i < used; i++) built->used().ptr[i] = args[i];
                    if(created && sourceResult) {
                        auto result = ssa[inst.results.get(ssa, 0)];
                        new (built->created().ptr) LowerValue(built, manifestOf(*result).type,
                                                              StringId());
                    }
                    return built;
                };

                auto recordResult = [&]() {
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto manifest = manifestOf(*result);
                    auto size = immediate(block, LowerType::Int64, U64(manifest.size));
                    auto allocation = block.addInst(lower, new (to.arena) LowerInstAlloca(
                        StringId(), size, manifest.align));
                    auto address = ((LowerInstSingle*)allocation)->created().ptr - lower;
                    values.add(result->id.index, address);
                    return address;
                };

                auto storeRecordResults = [&](LowerInstIntrinsic* built,
                                              LowerPtr<LowerValue> record, Size count) {
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto repr = reprOf(layout, result->occurrence.type, result->occurrence.shape);
                    for(Size i = 0; i < count; i++) {
                        auto field = repr.fields.get(decl, U16(i));
                        assertTrue(!field.isPacked());
                        block.addInst(lower, new (to.arena) LowerInstStore(
                            offsetAddress(block, record, field.offset),
                            built->created().ptr + i - lower, field.wordBytes));
                    }
                };

                switch(op) {
                    case mir::NativeOp::X86Nop: {
                        auto built = addIntrinsic(LowerIntrinsic::Nop, 0, 0, false);
                        block.addInst(lower, (LowerInst*)built);
                        return built;
                    }
                    case mir::NativeOp::X86Lretq: {
                        auto built = addIntrinsic(LowerIntrinsic::Lretq, 0, 1, false);
                        block.addInst(lower, (LowerInst*)built);
                        return built;
                    }
                    case mir::NativeOp::X86Cpuid: {
                        auto record = recordResult();
                        auto built = addIntrinsic(LowerIntrinsic::Cpuid, 4, 2, false);
                        for(Size i = 0; i < 4; i++) {
                            new (built->created().ptr + i) LowerValue(built, LowerType::Int32,
                                                                       StringId());
                        }
                        block.addInst(lower, (LowerInst*)built);
                        storeRecordResults(built, record, 4);
                        return nullptr;
                    }
                    default:
                        break;
                }

                LowerIntrinsic which = LowerIntrinsic::Rdtsc;
                switch(op) {
                    case mir::NativeOp::X86Rdtscp:      which = LowerIntrinsic::Rdtscp; break;
                    case mir::NativeOp::X86Rdtsc:       which = LowerIntrinsic::Rdtsc; break;
                    case mir::NativeOp::X86MFence:      which = LowerIntrinsic::MFence; break;
                    case mir::NativeOp::X86LFence:      which = LowerIntrinsic::LFence; break;
                    case mir::NativeOp::X86SFence:      which = LowerIntrinsic::SFence; break;
                    case mir::NativeOp::X86Pause:       which = LowerIntrinsic::Pause; break;
                    case mir::NativeOp::X86Prefetch:    which = LowerIntrinsic::Prefetch; break;
                    case mir::NativeOp::X86PrefetchNta: which = LowerIntrinsic::PrefetchNta; break;
                    case mir::NativeOp::X86Clflush:     which = LowerIntrinsic::Clflush; break;
                    case mir::NativeOp::X86Invlpg:      which = LowerIntrinsic::Invlpg; break;
                    case mir::NativeOp::X86Hlt:         which = LowerIntrinsic::Hlt; break;
                    case mir::NativeOp::X86Cli:         which = LowerIntrinsic::Cli; break;
                    case mir::NativeOp::X86Sti:         which = LowerIntrinsic::Sti; break;
                    case mir::NativeOp::X86Swapgs:      which = LowerIntrinsic::Swapgs; break;
                    case mir::NativeOp::X86Rdmsr:       which = LowerIntrinsic::Rdmsr; break;
                    case mir::NativeOp::X86Wrmsr:       which = LowerIntrinsic::Wrmsr; break;
                    case mir::NativeOp::X86Xgetbv:      which = LowerIntrinsic::Xgetbv; break;
                    case mir::NativeOp::X86In8:         which = LowerIntrinsic::In8; break;
                    case mir::NativeOp::X86In16:        which = LowerIntrinsic::In16; break;
                    case mir::NativeOp::X86In32:        which = LowerIntrinsic::In32; break;
                    case mir::NativeOp::X86Out8:        which = LowerIntrinsic::Out8; break;
                    case mir::NativeOp::X86Out16:       which = LowerIntrinsic::Out16; break;
                    case mir::NativeOp::X86Out32:       which = LowerIntrinsic::Out32; break;
                    case mir::NativeOp::X86Ins32:       which = LowerIntrinsic::Ins32; break;
                    case mir::NativeOp::X86Outs32:      which = LowerIntrinsic::Outs32; break;
                    case mir::NativeOp::X86Interrupt:   which = LowerIntrinsic::Interrupt; break;
                    case mir::NativeOp::X86Lidt:        which = LowerIntrinsic::Lidt; break;
                    case mir::NativeOp::X86Lgdt:        which = LowerIntrinsic::Lgdt; break;
                    case mir::NativeOp::X86MovDs:       which = LowerIntrinsic::MovDs; break;
                    case mir::NativeOp::X86MovEs:       which = LowerIntrinsic::MovEs; break;
                    case mir::NativeOp::X86MovFs:       which = LowerIntrinsic::MovFs; break;
                    case mir::NativeOp::X86MovGs:       which = LowerIntrinsic::MovGs; break;
                    case mir::NativeOp::X86MovSs:       which = LowerIntrinsic::MovSs; break;
                    case mir::NativeOp::X86ReadCr0:     which = LowerIntrinsic::ReadCr0; break;
                    case mir::NativeOp::X86ReadCr2:     which = LowerIntrinsic::ReadCr2; break;
                    case mir::NativeOp::X86ReadCr3:     which = LowerIntrinsic::ReadCr3; break;
                    case mir::NativeOp::X86ReadCr4:     which = LowerIntrinsic::ReadCr4; break;
                    case mir::NativeOp::X86WriteCr0:    which = LowerIntrinsic::WriteCr0; break;
                    case mir::NativeOp::X86WriteCr3:    which = LowerIntrinsic::WriteCr3; break;
                    case mir::NativeOp::X86WriteCr4:    which = LowerIntrinsic::WriteCr4; break;
                    default:
                        assertTrue("not an x86 native operation" == nullptr);
                        return nullptr;
                }

                auto created = Size(inst.results.size());
                auto used = lowerIntrinsicDesc(which).args;
                auto built = addIntrinsic(which, created, used, created != 0);
                auto value = block.addInst(lower, (LowerInst*)built);
                if(created) values.add(ssa[inst.results.get(ssa, 0)]->id.index,
                                       built->created().ptr - lower);
                return value;
            };

            switch(written.op) {
                case mir::NativeOp::CopyMemory:
                    block.addInst(lower, new (to.arena) LowerInstCopy(args[0], args[1], args[2]));
                    break;

                // `setMemory` is written `(to, value, count)` and the instruction takes
                // `(to, count, pattern)`, which is the order its printed form uses.
                case mir::NativeOp::SetMemory:
                    block.addInst(lower, new (to.arena) LowerInstSetPattern(args[0], args[2],
                                                                            args[1]));
                    break;

                // The transfers that stop at a lane count, one lower kind each - see
                // LowerInst::VecLoadUpTo.
                case mir::NativeOp::VecLoadUpTo: {
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstVecLoadUpTo(
                        StringId(), manifestOf(*result).type, args[0], args[1]));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                case mir::NativeOp::VecStoreUpTo:
                    block.addInst(lower, new (to.arena) LowerInstVecStoreUpTo(args[0], args[1], args[2]));
                    break;

                case mir::NativeOp::Syscall: {
                    // The kernel is the callee, so there is no function operand: the number is
                    // operand zero, exactly as the lower IR's own syscall form has it.
                    auto created = U32(inst.results.size());

                    auto value = call(lower, to, block, created, args.size(),
                                      LowerCallType::Syscall, [&](LowerInstCall* built) {
                        for(Size i = 0; i < created; i++) {
                            auto result = ssa[inst.results.get(ssa, i)];
                            new (built->created().ptr + i) LowerValue(built,
                                                                       manifestOf(*result).type,
                                                                       StringId());
                        }

                        for(Size i = 0; i < args.size(); i++) built->used()[i] = args[i];
                    });

                    for(Size i = 0; i < created; i++) {
                        values.add(ssa[inst.results.get(ssa, i)]->id.index,
                                   value->created().ptr + i - lower);
                    }

                    break;
                }

                /*
                 * Starting a thread - a backend pseudo whose encoder is the `clone` system call plus
                 * the child's entry sequence, and the one `Native` row whose operands are not the
                 * ones MIR wrote.
                 *
                 * **The entry's pair is split here**, which is why this arm is longer than the three
                 * above it. The declaration takes an ordinary function of the language - a closure
                 * included - so what MIR carries is the `{code, env}` value; the child needs the
                 * code to jump to and the environment to pass as that code's first argument, which
                 * are two words at two offsets, and this file is where a function value is known to
                 * be that. `CallThrough::Value` above reads the same pair the same way.
                 *
                 * The two loads happen in the **parent**, in front of the intrinsic, and what
                 * crosses to the child is two words the encoder writes onto its stack. The operand
                 * order is the backend's - `(flags, stackTop, code, argument, threadId, env)` - and
                 * is the shipping stage's, so `LowerIntrinsic::CloneThread` has one caller shape.
                 */
                case mir::NativeOp::CloneThread: {
                    auto code = wordAt(block, args[2], layout.target.funCodeOffset(),
                                       LowerType::Pointer, layout.target.pointerSize);
                    auto environment = wordAt(block, args[2], layout.target.funEnvOffset(),
                                              LowerType::Pointer, layout.target.pointerSize);

                    LowerPtr<LowerValue> operands[] = { args[0], args[1], code, args[3], args[4],
                                                        environment };
                    auto count = Size(6);

                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto built = new (allocTrailing<LowerInstIntrinsic, LowerValue, LowerPtr<LowerValue>>(
                        to.arena, sizeof(LowerValue) + sizeof(LowerPtr<LowerValue>) * count))
                        LowerInstIntrinsic(LowerIntrinsic::CloneThread, 1, count);
                    for(Size i = 0; i < count; i++) built->used().ptr[i] = operands[i];
                    new (built->created().ptr) LowerValue(built, manifestOf(*result).type,
                                                          StringId());

                    auto value = block.addInst(lower, (LowerInst*)built);
                    values.add(result->id.index, ((LowerInstIntrinsic*)value)->created().ptr - lower);
                    break;
                }

                /*
                 * Preparing a stack - `LowerIntrinsic::StackInit`, and `CloneThread` above with one
                 * operand fewer.
                 *
                 * The entry's `{code, env}` pair is split here for that arm's reason: the
                 * declaration takes an ordinary function of the language, so a closure is admitted
                 * and both words have to reach the prepared stack, and this file is where a function
                 * value is known to be two words at two offsets. The operand order is the backend's -
                 * `(stackTop, code, environment, argument)`.
                 */
                case mir::NativeOp::StackInit: {
                    auto code = wordAt(block, args[2], layout.target.funCodeOffset(),
                                       LowerType::Pointer, layout.target.pointerSize);
                    auto environment = wordAt(block, args[2], layout.target.funEnvOffset(),
                                              LowerType::Pointer, layout.target.pointerSize);

                    LowerPtr<LowerValue> operands[] = { args[0], code, environment, args[1] };
                    auto count = Size(4);

                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto built = new (allocTrailing<LowerInstIntrinsic, LowerValue, LowerPtr<LowerValue>>(
                        to.arena, sizeof(LowerValue) + sizeof(LowerPtr<LowerValue>) * count))
                        LowerInstIntrinsic(LowerIntrinsic::StackInit, 1, count);
                    for(Size i = 0; i < count; i++) built->used().ptr[i] = operands[i];
                    new (built->created().ptr) LowerValue(built, manifestOf(*result).type,
                                                          StringId());

                    auto value = block.addInst(lower, (LowerInst*)built);
                    values.add(result->id.index, ((LowerInstIntrinsic*)value)->created().ptr - lower);
                    break;
                }

                /*
                 * The switch itself - a **call**, and the one `Native` row that becomes one.
                 *
                 * Not an intrinsic, and the reason is the register effect rather than the
                 * instruction: the other context runs at this point, so nothing the allocator handed
                 * out survives it, and that is a statement a convention makes and a form cannot -
                 * a form's clobbers land in front of its operands, where a call's land behind them.
                 * `LowerCallType::StackSwitch` is the convention and its note is the argument.
                 *
                 * There is no callee operand, exactly as a system call has none: what runs next is
                 * whatever the target stack's top word holds, which is not an address this IR names.
                 * The three operands are `(target, save, transfer)` and the result is the transfer
                 * word whichever context switches back supplies.
                 */
                case mir::NativeOp::StackSwitch: {
                    auto created = U32(inst.results.size());

                    auto value = call(lower, to, block, created, args.size(),
                                      LowerCallType::StackSwitch, [&](LowerInstCall* built) {
                        for(Size i = 0; i < created; i++) {
                            auto result = ssa[inst.results.get(ssa, i)];
                            new (built->created().ptr + i) LowerValue(built,
                                                                       manifestOf(*result).type,
                                                                       StringId());
                        }

                        for(Size i = 0; i < args.size(); i++) built->used()[i] = args[i];
                    });

                    for(Size i = 0; i < created; i++) {
                        values.add(ssa[inst.results.get(ssa, i)]->id.index,
                                   value->created().ptr + i - lower);
                    }

                    break;
                }

                case mir::NativeOp::X86Nop:
                case mir::NativeOp::X86Lretq:
                case mir::NativeOp::X86Cpuid:
                case mir::NativeOp::X86Rdtscp:
                case mir::NativeOp::X86Rdtsc:
                case mir::NativeOp::X86MFence:
                case mir::NativeOp::X86LFence:
                case mir::NativeOp::X86SFence:
                case mir::NativeOp::X86Pause:
                case mir::NativeOp::X86Prefetch:
                case mir::NativeOp::X86PrefetchNta:
                case mir::NativeOp::X86Clflush:
                case mir::NativeOp::X86Invlpg:
                case mir::NativeOp::X86Hlt:
                case mir::NativeOp::X86Cli:
                case mir::NativeOp::X86Sti:
                case mir::NativeOp::X86Swapgs:
                case mir::NativeOp::X86Rdmsr:
                case mir::NativeOp::X86Wrmsr:
                case mir::NativeOp::X86Xgetbv:
                case mir::NativeOp::X86In8:
                case mir::NativeOp::X86In16:
                case mir::NativeOp::X86In32:
                case mir::NativeOp::X86Out8:
                case mir::NativeOp::X86Out16:
                case mir::NativeOp::X86Out32:
                case mir::NativeOp::X86Ins32:
                case mir::NativeOp::X86Outs32:
                case mir::NativeOp::X86Interrupt:
                case mir::NativeOp::X86Lidt:
                case mir::NativeOp::X86Lgdt:
                case mir::NativeOp::X86MovDs:
                case mir::NativeOp::X86MovEs:
                case mir::NativeOp::X86MovFs:
                case mir::NativeOp::X86MovGs:
                case mir::NativeOp::X86MovSs:
                case mir::NativeOp::X86ReadCr0:
                case mir::NativeOp::X86ReadCr2:
                case mir::NativeOp::X86ReadCr3:
                case mir::NativeOp::X86ReadCr4:
                case mir::NativeOp::X86WriteCr0:
                case mir::NativeOp::X86WriteCr3:
                case mir::NativeOp::X86WriteCr4:
                    lowerX86(written.op);
                    break;

                /*
                 * The x86 family - native.def's system rows, and a mapping and nothing else.
                 *
                 * Each is an instruction the lower IR already has, so there is no expansion here
                 * and no width to mask: `crc32` is declared at 32 and 64 bits only and the SHA
                 * steps are named stages of a named algorithm over a four-lane word vector. The
                 * `sha1rnds4` round group arrives as the *row*, which is what makes this a
                 * translation between two enums rather than a constant read out of an operand -
                 * `LowerSha` says the same thing from its side.
                 */
                case mir::NativeOp::Crc32: {
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstBinary(
                        StringId(), manifestOf(*result).type, args[0], args[1], LowerInst::Crc32));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                // No operands and no result: what it changes is processor state nothing in this IR
                // names, which is why the row answers `{}` and nothing is recorded for it.
                case mir::NativeOp::VZeroUpper:
                    block.addInst(lower, new (to.arena) LowerInstVZeroUpper());
                    break;

                case mir::NativeOp::VZeroAll:
                    block.addInst(lower, new (to.arena) LowerInstVZeroAll());
                    break;

                case mir::NativeOp::Sha1Message1:
                case mir::NativeOp::Sha1Message2:
                case mir::NativeOp::Sha1NextE:
                case mir::NativeOp::Sha1Rounds0:
                case mir::NativeOp::Sha1Rounds1:
                case mir::NativeOp::Sha1Rounds2:
                case mir::NativeOp::Sha1Rounds3:
                case mir::NativeOp::Sha256Message1:
                case mir::NativeOp::Sha256Message2: {
                    auto op = LowerSha::Sha1Msg1;
                    switch(written.op) {
                        case mir::NativeOp::Sha1Message2:   op = LowerSha::Sha1Msg2; break;
                        case mir::NativeOp::Sha1NextE:      op = LowerSha::Sha1NextE; break;
                        case mir::NativeOp::Sha1Rounds0:    op = LowerSha::Sha1Rounds0; break;
                        case mir::NativeOp::Sha1Rounds1:    op = LowerSha::Sha1Rounds1; break;
                        case mir::NativeOp::Sha1Rounds2:    op = LowerSha::Sha1Rounds2; break;
                        case mir::NativeOp::Sha1Rounds3:    op = LowerSha::Sha1Rounds3; break;
                        case mir::NativeOp::Sha256Message1: op = LowerSha::Sha256Msg1; break;
                        case mir::NativeOp::Sha256Message2: op = LowerSha::Sha256Msg2; break;
                        default: break;
                    }

                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstShaBinary(
                        StringId(), manifestOf(*result).type, args[0], args[1], op));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                // The third operand is `xmm0` on the machine, which is the x64 form's constraint
                // and not this seam's - here it is an ordinary third use.
                case mir::NativeOp::Sha256Rounds: {
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstSha256Rounds(
                        StringId(), manifestOf(*result).type, args[0], args[1], args[2]));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                /*
                 * AES-NI and PCLMULQDQ. The four rounds and the multiply are one lower kind, whose
                 * `LowerCrypto` member is the row - or, for the multiply, the selector the node
                 * carries as its `immediate`, which is one of the four values the expansion allowed.
                 */
                case mir::NativeOp::AesEncrypt:
                case mir::NativeOp::AesEncryptLast:
                case mir::NativeOp::AesDecrypt:
                case mir::NativeOp::AesDecryptLast:
                case mir::NativeOp::CarrylessMultiply: {
                    auto op = LowerCrypto::AesEnc;
                    switch(written.op) {
                        case mir::NativeOp::AesEncryptLast: op = LowerCrypto::AesEncLast; break;
                        case mir::NativeOp::AesDecrypt:     op = LowerCrypto::AesDec; break;
                        case mir::NativeOp::AesDecryptLast: op = LowerCrypto::AesDecLast; break;
                        case mir::NativeOp::CarrylessMultiply:
                            op = carrylessOfSelector(written.immediate);
                            break;
                        default: break;
                    }

                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstCryptoBinary(
                        StringId(), manifestOf(*result).type, args[0], args[1], op));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                case mir::NativeOp::AesInverseMix: {
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstAesInverseMix(
                        StringId(), manifestOf(*result).type, args[0]));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                case mir::NativeOp::AesKeygenAssist: {
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstAesKeygenAssist(
                        StringId(), manifestOf(*result).type, args[0], written.immediate));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                /*
                 * The AVX-512 float operations, two lower kinds with the row as the operation and
                 * the node's `immediate` as the instruction's trailing byte. `vgetexp` and
                 * `vscalef` have no byte, and the node carries a zero nothing reads.
                 */
                case mir::NativeOp::GetExponent:
                case mir::NativeOp::GetMantissa:
                case mir::NativeOp::ReduceFloat: {
                    auto kind = written.op == mir::NativeOp::GetExponent ? LowerInst::GetExponent
                              : written.op == mir::NativeOp::GetMantissa ? LowerInst::GetMantissa
                                                                         : LowerInst::FpReduce;

                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstFloatAdjust(
                        kind, StringId(), manifestOf(*result).type, args[0], written.immediate));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                case mir::NativeOp::ScaleFloat:
                case mir::NativeOp::RangeFloat: {
                    auto kind = written.op == mir::NativeOp::ScaleFloat ? LowerInst::ScaleFloat
                                                                        : LowerInst::RangeFloat;

                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstFloatCombine(
                        kind, StringId(), manifestOf(*result).type, args[0], args[1],
                        written.immediate));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                /*
                 * The four widening multiply-accumulates, one lower kind each - see
                 * `LowerInstMulAccumulate`, where the reason the operation is the kind rather than a
                 * field is written.
                 */
                case mir::NativeOp::VecDotBytes:
                case mir::NativeOp::VecDotWords:
                case mir::NativeOp::MulAdd52Low:
                case mir::NativeOp::MulAdd52High: {
                    auto kind = written.op == mir::NativeOp::VecDotBytes ? LowerInst::DotBytes
                              : written.op == mir::NativeOp::VecDotWords ? LowerInst::DotWords
                              : written.op == mir::NativeOp::MulAdd52Low ? LowerInst::MulAdd52Lo
                                                                         : LowerInst::MulAdd52Hi;

                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstMulAccumulate(
                        kind, StringId(), manifestOf(*result).type, args[0], args[1], args[2]));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }

                // The three lane movements, one lower kind with the row as its `LowerLaneMove`.
                case mir::NativeOp::VecCompress:
                case mir::NativeOp::VecExpand:
                case mir::NativeOp::VecLookup: {
                    auto move = written.op == mir::NativeOp::VecCompress ? LowerLaneMove::Compress
                              : written.op == mir::NativeOp::VecExpand   ? LowerLaneMove::Expand
                                                                         : LowerLaneMove::Lookup;

                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto value = block.addInst(lower, new (to.arena) LowerInstVecMove(
                        StringId(), manifestOf(*result).type, args[0], args[1], move));

                    values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                    break;
                }
            }

            break;
        }
        case Inst::Fma: {
            auto& written = (InstFma&)inst;
            auto lhs = mapped(written.lhs.value());
            auto rhs = mapped(written.rhs.value());
            auto addend = mapped(written.addend.value());
            if(!lhs || !rhs || !addend) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto value = block.addInst(lower, new (to.arena) LowerInstFma(
                StringId(), manifestOf(*result).type, lhs - lower, rhs - lower, addend - lower));

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }

        /*
         * A branch-free choice - the one arithmetic row whose condition may be a mask, and a rename
         * like the vector rows below it.
         *
         * The lower IR takes the arms before the condition, which is the order its own printed form
         * has; the MIR node takes the condition first, which is the order the language writes. The
         * swap is here and nowhere else.
         */
        case Inst::Select: {
            auto& written = (InstSelect&)inst;
            auto condition = mapped(written.condition.value());
            auto whenTrue = mapped(written.whenTrue.value());
            auto whenFalse = mapped(written.whenFalse.value());
            if(!condition || !whenTrue || !whenFalse) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto value = block.addInst(lower, new (to.arena) LowerInstSelect(
                StringId(), whenTrue - lower, whenFalse - lower, condition - lower,
                manifestOf(*result).type));

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }

        /*
         * The four vector operations - a rename, and nothing to decide.
         *
         * That is by construction rather than by luck: a runtime lane index and a runtime shuffle
         * pattern were both refused where the call was written, so each arrives as a field, and the
         * natural lane count was spent at the type. What is left below is the two places a *narrow
         * lane* needs its sign put back, which is the seam `width.cpp` states.
         */
        case Inst::VecSplat: {
            auto& written = (InstVecSplat&)inst;
            auto from = mapped(written.from.value());
            if(!from) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto value = block.addInst(lower, new (to.arena) LowerInstVecSplat(
                StringId(), manifestOf(*result).type, from - lower));

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }

        case Inst::VecLane: {
            auto& written = (InstVecLane&)inst;
            auto from = mapped(written.from.value());
            if(!from) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = manifestOf(*result);

            auto read = block.addInst(lower, new (to.arena) LowerInstVecLane(
                StringId(), manifest.type, from - lower, U8(written.lane)));

            auto value = &((LowerInstSingle*)read)->result - lower;

            /*
             * The sign, put back on a lane narrower than the register it arrives in.
             *
             * The lower IR's contract is that an 8- or 16-bit lane is read **zero-extended** - which
             * is what `pextrb` and `pextrw` do - because the lane type it arrives as states a width
             * and not a signedness. `LowerLane::Int8` is eight bits and nothing else, so no backend
             * can know which extension was wanted, and this is the last tier that does.
             *
             * Only the signed half is asked for: the unsigned one is what the contract already
             * guarantees, so putting it in range would be an `and` against bits that cannot be set.
             */
            auto width = narrowWidthOf(result->type());
            if(width && width.isSigned) value = putInRange(block, value, width, StringId());

            values.add(result->id.index, value);
            break;
        }

        case Inst::VecWithLane: {
            auto& written = (InstVecLane&)inst;
            auto from = mapped(written.from.value());
            auto lane = mapped(written.value.value());
            if(!from || !lane) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto value = block.addInst(lower, new (to.arena) LowerInstVecLane(
                StringId(), manifestOf(*result).type, from - lower, U8(written.lane), lane - lower));

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }

        case Inst::VecShuffle: {
            /*
             * The one that is not a plain construction: the lower IR's shuffle keeps its pattern in
             * the allocation past its own operands, so the space is reserved before the instruction
             * is built rather than pushed into it afterwards.
             */
            auto& written = (InstVecShuffle&)inst;
            auto left = mapped(written.left.value());
            auto right = mapped(written.right.value());
            if(!left || !right) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto type = manifestOf(*result).type;

            auto memory = allocTrailing<LowerInstVecShuffle>(to.arena, LowerInstVecShuffle::patternBytes(type));
            auto lowered = new (memory) LowerInstVecShuffle(StringId(), type, left - lower,
                                                            right - lower);
            auto pattern = lowered->pattern();

            for(Size i = 0; i < pattern.length; i++) {
                pattern.ptr[i] = i < written.pattern.size() ? written.pattern.get(ssa, i) : 0;
            }

            block.addInst(lower, lowered);
            values.add(result->id.index, &lowered->result - lower);
            break;
        }

        case Inst::VecReduce: {
            auto& written = (InstVecReduce&)inst;
            auto from = mapped(written.from.value());
            if(!from) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = manifestOf(*result);

            /*
             * Where the signed/unsigned split is made. This tier has one `Min` and one `Max` because
             * signedness is in the lane *type*; the lower IR has `IMin`/`IMax` beside them because
             * by then a type is a lane kind and a count and has forgotten it.
             */
            auto lane = mir::laneTypeOf(decl, ssa[written.from.value()]->type());
            auto isSigned = lane && decl[lane]->kind == mir::Type::Int &&
                            ((mir::IntType*)decl[lane])->isSigned;

            auto reduce = LowerReduce::Add;
            switch(written.reduce) {
                case mir::ReduceOp::Add: reduce = LowerReduce::Add; break;
                case mir::ReduceOp::Mul: reduce = LowerReduce::Mul; break;
                case mir::ReduceOp::Min: reduce = isSigned ? LowerReduce::IMin : LowerReduce::Min; break;
                case mir::ReduceOp::Max: reduce = isSigned ? LowerReduce::IMax : LowerReduce::Max; break;
                case mir::ReduceOp::And: reduce = LowerReduce::And; break;
                case mir::ReduceOp::Or:  reduce = LowerReduce::Or; break;

                // The one row with no signedness to read and no lane type to read it off: what it
                // answers is an index into the lanes rather than one of them.
                case mir::ReduceOp::FirstSet: reduce = LowerReduce::FirstSet; break;

                // The movemask, which is the one kind that arrives already named: `Native.bits` is
                // the only producer and the lower row is the same operation, so there is nothing to
                // choose here the way `Min` chooses on signedness.
                case mir::ReduceOp::Bits: reduce = LowerReduce::Bits; break;
                case mir::ReduceOp::Count: assertTrue(false); break;
            }

            // The movemask answers the lanes at the width they fit in, which is the only width the
            // lower kind has for it, and is widened to the `Size` the declaration states after.
            auto answered = reduce == LowerReduce::Bits ? maskBitsType(from->type) : manifest.type;

            auto folded = block.addInst(lower, new (to.arena) LowerInstVecReduce(
                StringId(), answered, from - lower, reduce));

            auto value = &((LowerInstSingle*)folded)->result - lower;

            if(answered != manifest.type) {
                auto widened = block.addInst(lower, new (to.arena) LowerInstCast(
                    StringId(), manifest.type, value, false, false));
                value = &((LowerInstSingle*)widened)->result - lower;
            }

            // A reduction of a narrow lane answers that lane zero-extended, on the same terms a lane
            // read does - so it gets its sign back the same way. `firstset` answers an integer and
            // this asks nothing of it.
            auto width = narrowWidthOf(result->type());
            if(width && width.isSigned) value = putInRange(block, value, width, StringId());

            values.add(result->id.index, value);
            break;
        }

        /*
         * One atomic operation - the SSA instruction carried down to the lower IR's own six.
         *
         * Two of the rows are not atomic instructions and are handled first, which is where the
         * claim that an `Atomic(a)`'s storage *is* its content's bytes is spent: **constructing**
         * one is frame storage and an ordinary store, because nothing can be sharing a location that
         * is still being built - what a sharer holds is a borrow, and a borrow of a value under
         * construction does not exist. **Consuming** one is an ordinary load, because owning it
         * exclusively is the proof that nobody is sharing it. Making either of them ordered would
         * cost an edge published to nobody.
         *
         * Everything below them takes the location as an address and states the access width, which
         * is the content's manifested size. The order and the strength travel down unchanged; the
         * translation of the two enums is the one switch this tier owes the lower IR, and it is
         * `lowerOrder` beside this.
         */
        case Inst::Atomic: {
            auto& written = (InstAtomic&)inst;

            SmallArray<LowerPtr<LowerValue>, 4> args;
            for(auto used: written.arguments.contents(ssa)) {
                auto value = mapped(used.value());
                if(!value) return;
                args.push(value - lower);
            }

            if(written.op == mir::AtomicOp::New) {
                auto result = ssa[inst.results.get(ssa, 0)];
                auto storage = frameStorage(block, result->occurrence);

                install(block, storage, *ssa[written.arguments.get(ssa, 0).value()]);
                values.add(result->id.index, storage);
                break;
            }

            if(written.op == mir::AtomicOp::IntoValue) {
                auto result = ssa[inst.results.get(ssa, 0)];
                auto manifest = manifestOf(*result);

                auto value = block.addInst(lower, new (to.arena) LowerInstLoad(
                    args[0], StringId(), manifest.type, manifest.size, manifest.isSigned));

                values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                break;
            }

            if(written.op == mir::AtomicOp::Fence) {
                block.addInst(lower, new (to.arena) LowerInstFence(lowerOrder(written.order)));
                break;
            }

            if(written.op == mir::AtomicOp::SpinHint) {
                block.addInst(lower, new (to.arena) LowerInstSpinHint());
                break;
            }

            /*
             * The width, taken from the **value** rather than from the location.
             *
             * A store and an update have their content in hand and a load has it as its result, so
             * one of the two ends always states it - and the location's own manifestation is the
             * address of an in-memory value, which measures the pointer rather than what it names.
             */
            auto accessOf = [&](Value& value) {
                auto manifest = manifestOf(value);
                return manifest;
            };

            switch(written.op) {
                case mir::AtomicOp::Load: {
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto manifest = accessOf(*result);

                    auto value = block.addInst(lower, new (to.arena) LowerInstAtomicLoad(
                        args[0], StringId(), manifest.type, manifest.size, manifest.isSigned,
                        lowerOrder(written.order), written.isVolatile));

                    values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                    break;
                }

                case mir::AtomicOp::Store: {
                    auto manifest = accessOf(*ssa[written.arguments.get(ssa, 1).value()]);

                    block.addInst(lower, new (to.arena) LowerInstAtomicStore(
                        args[0], args[1], manifest.size, lowerOrder(written.order),
                        written.isVolatile));
                    break;
                }

                case mir::AtomicOp::Compare: {
                    auto previous = ssa[inst.results.get(ssa, 0)];
                    auto manifest = accessOf(*previous);

                    auto cas = block.addInst(lower, new (to.arena) LowerInstAtomicCas(
                        StringId(), StringId(), manifest.type, args[0], args[1], args[2],
                        manifest.size, written.weak, lowerOrder(written.order),
                        lowerOrder(written.failure)));

                    // Result zero is `previous` and result one is `exchanged` - the order
                    // `LowerInstAtomicCas` embeds them in, which `AtomicOk` below reads by index.
                    auto value = ((LowerInstAtomicCas*)cas)->created().ptr - lower;

                    if(auto width = narrowWidthOf(previous->type())) {
                        value = putInRange(block, value, width, StringId());
                    }

                    values.add(previous->id.index, value);
                    break;
                }

                default: {
                    // The read-modify-writes, which are one shape - `atomicRmwOp` is what says which
                    // arithmetic, and it is total over exactly this group.
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto manifest = accessOf(*result);

                    auto rmw = block.addInst(lower, new (to.arena) LowerInstAtomicRmw(
                        args[0], args[1], StringId(), manifest.type, manifest.size,
                        atomicRmwOp(written.op), lowerOrder(written.order)));

                    auto value = &((LowerInstSingle*)rmw)->result - lower;

                    /*
                     * And back into the declared width - the same normalization every other narrow
                     * producer ends with, and the reason it is here rather than in `width.cpp`'s
                     * list is that a machine's atomic answers the *register* it was handed.
                     *
                     * `lock xadd byte [m], r8` writes the low byte of its operand register and
                     * leaves that register's own high bits standing; `lock cmpxchg` at a byte leaves
                     * the expected value's. So a `U8` at 250 less 10 came back as 0xFFFFFF04 with a
                     * correct byte under three bytes of the operand - `Atomic.wrapping` in
                     * `lib/Atomic/Atomic.test.native.yana`, which is the case that found it in the
                     * shipping stage too (`lower_calc.cpp`, where the same four spellings are
                     * listed).
                     *
                     * **The load is not among them**: it states its signedness on the instruction
                     * and both backends widen as they read, which is one instruction rather than
                     * two.
                     */
                    if(auto width = narrowWidthOf(result->type())) {
                        value = putInRange(block, value, width, StringId());
                    }

                    values.add(result->id.index, value);
                    break;
                }
            }

            break;
        }

        /*
         * Whether the compare-exchange stored - the **second** created value of the instruction the
         * `Atomic` beside it produced.
         *
         * Reached through the lower instruction rather than through a second map, because the two
         * results are embedded in one instruction and `created()` is what numbers them: `previous`
         * is zero and `exchanged` is one, everywhere.
         */
        case Inst::AtomicOk: {
            auto& written = (InstAtomicOk&)inst;
            auto previous = mapped(written.cas.value());
            if(!previous) return;

            /*
             * Walked back to the instruction, because a narrow compared type puts the mask or the
             * shift pair in front of it: what the compare's own value maps to is then the
             * normalization rather than the `cmpxchg`, and the second result lives on the
             * instruction. Each step reads its predecessor as operand zero, which is the way back.
             */
            while(previous->inst()->kind != LowerInst::AtomicCas) {
                previous = lower[previous->inst()->used()[0]];
            }

            auto result = ssa[inst.results.get(ssa, 0)];
            auto cas = (LowerInstAtomicCas*)previous->inst();

            values.add(result->id.index, &cas->exchanged - lower);
            break;
        }

        case Inst::Call: {
            auto& written = (InstCall&)inst;

            /*
             * The target as a value, which is what `LowerInstCall` takes in its first used position.
             *
             * A direct call names a function and an indirect one names any other pointer, so the
             * call itself needs no second form - which is the note this file already carried before
             * there was a second thing to put there. Which applies is `InstCall::through`, decided
             * once by `callTarget` and carried down rather than derived here.
             */
            LowerPtr<LowerValue> callee = nullptr;

            /*
             * What the callee reads its own layouts out of, where it takes one.
             *
             * A direct call to a body compiled once passes the environment its argument list
             * decided, as a table in the image. A slot dispatch passes none - `noEnvironment`, where
             * the reason is written.
             */
            LowerPtr<LowerValue> environment = nullptr;

            /*
             * `-check-locations` - the located check this call becomes, and null everywhere else.
             *
             * Read here so that the whole substitution is one binding: the callee below is the
             * located declaration, the parameter list the argument loop adapts against is the
             * located one, and the four constants are appended after it. See `locatedCheck`, where
             * the reason this is the lowering's decision and not the resolver's is written.
             */
            auto located = written.through == mir::CallThrough::Declaration
                ? locatedCheck(written.callee) : DeclPtr<mir::Function>(nullptr);

            auto target = located ? located : written.callee;

            switch(written.through) {
                case mir::CallThrough::Declaration: {
                    auto found = functions.getValue(target.offset);
                    if(!found) {
                        context.diagnostics.error(
                            "internal: a call reached a callee that was not lowered"_v, inst.source);
                        return;
                    }

                    callee = ((LowerInstSingle*)block.addInst(lower, new (to.arena) LowerInstFun(
                        decl[target]->name, found.unwrap())))->created().ptr - lower;

                    if(written.environment) {
                        auto table = environmentFor(written.environment, inst.source);
                        if(!table) return;

                        environment = ((LowerInstSingle*)block.addInst(lower,
                            new (to.arena) LowerInstGlobal(lower[table]->name, table)))
                            ->created().ptr - lower;
                    }

                    break;
                }

                /*
                 * A class member, through the witness the caller put in the requirement's slot.
                 *
                 * Two loads: the table out of the environment, and the member out of the table. That
                 * the member's *position* is the class's declaration order is the whole of what a
                 * witness table is, and `witnessFor` fills it in that order for the same reason
                 * `freezeSchema` numbers slots in one - the two ends never describe each other.
                 */
                case mir::CallThrough::ClassSlot: {
                    auto slot = requirementSlot(mir::GenSlotKind::Class, written.requirement);
                    callee = witnessMember(block, slotPointer(block, slot), written.member);
                    environment = noEnvironment(block);
                    break;
                }

                /*
                 * A value the `Call` requirement typed - the same two loads, at the one cell the
                 * compiler synthesizes for a structural class (`CallThrough::CallWitness`).
                 *
                 * The value itself is argument **zero** and is already in the list, put there by
                 * the conversion exactly as a site type's environment is. It cannot ride in the
                 * environment word: that word is where an erased callee's own generic environment
                 * goes, and what the cell holds here is a thunk with a generic signature of its own.
                 */
                case mir::CallThrough::CallWitness: {
                    auto slot = requirementSlot(mir::GenSlotKind::Class, written.requirement);
                    callee = witnessMember(block, slotPointer(block, slot), 0);
                    environment = noEnvironment(block);
                    break;
                }

                // A callable requirement, which is one load: the slot holds the function itself
                // rather than a table, because a requirement names one callable and a class names a
                // list.
                case mir::CallThrough::FunctionSlot: {
                    auto slot = requirementSlot(mir::GenSlotKind::Function, written.requirement);
                    callee = slotPointer(block, slot);
                    environment = noEnvironment(block);
                    break;
                }

                /*
                 * A function **value** the body is holding - G7.
                 *
                 * Two loads out of the pair, and the second one goes into the *same* leading
                 * argument the erased form puts a generic environment in. That is not a collision:
                 * argument zero is "what the callee needs that the arguments do not say", and a
                 * body compiled once needs its layouts while a closure needs its captures. One
                 * function value can never want both, because a lambda over a body compiled once
                 * reaches it through a thunk that supplies the environment out of its own captures.
                 */
                case mir::CallThrough::Value: {
                    auto pair = mapped(written.target.value());
                    if(!pair) return;

                    auto targetType = ssa[written.target.value()]->occurrence.type;
                    auto raw = decl[targetType]->kind == mir::Type::Fun &&
                               ((mir::FunType*)decl[targetType])->convention;
                    if(raw) {
                        callee = pair - lower;
                    } else {
                        callee = wordAt(block, pair - lower, layout.target.funCodeOffset(),
                                        LowerType::Pointer, layout.target.pointerSize);
                        environment = wordAt(block, pair - lower, layout.target.funEnvOffset(),
                                             LowerType::Pointer, layout.target.pointerSize);
                    }
                    break;
                }
            }

            /*
             * The arguments, each adapted to what the *position* takes.
             *
             * Exactly one direction ever needs an instruction, and it is the concrete-to-erased one:
             * a body compiled once reaches every value of a type parameter through a pointer, so a
             * caller that knows the type and is holding it in a register has to give it storage
             * first. `thunkFor` is the same adaptation read from the other end - it loads where this
             * stores - and the two are the whole of the boundary.
             *
             * Only a call that names a **declaration** can need it. Every other form is reached from
             * inside an erased body, where a value at a type parameter is already an address, and a
             * call through a function value is checked against one signature that both ends read.
             */
            auto declared = written.through == mir::CallThrough::Declaration ||
                            written.through == mir::CallThrough::ClassSlot
                ? decl[target] : nullptr;

            SmallArray<LowerPtr<LowerValue>, 8> passed;
            SmallArray<PackedBorrow, 4> commits;
            U32 position = 0;

            // Which records the callee takes and gives back as their fields - only ever a direct
            // call's question, since a slot names a thunk whose signature is the promise's.
            auto shape = written.through == mir::CallThrough::Declaration ? callShapeOf(target)
                                                                         : nullptr;

            for(auto used: written.arguments.contents(ssa)) {
                auto value = mapped(used.value());
                if(!value) return;

                auto supplied = ssa[used.value()];
                auto place = value - lower;

                if(declared && position < declared->args.size()) {
                    auto& parameter = *decl[declared->args.get(decl, position)];

                    if(arrivesAsAddress(layout, parameter.occurrence) &&
                       !arrivesAsAddress(layout, supplied->occurrence)) {
                        place = frameStorage(block, supplied->occurrence);
                        install(block, place, *supplied);
                    }
                }

                /*
                 * A **narrow reference into a position that cannot hold one** - the boundary
                 * Design.md's tier 2 does not cross, flattened.
                 *
                 * The position takes a shifted reference only where its declared pointee is narrow
                 * *there*, which a type parameter never is: a body compiled once reaches a `&a`
                 * through a plain address and sizes it out of a descriptor. A call with no
                 * declaration to read is treated the same way, which is conservative in the one
                 * direction that is safe - a temporary is a real address at shift zero, so a callee
                 * expecting either form reads it correctly.
                 */
                if(isNarrowReference(layout, supplied->occurrence)) {
                    auto held = declared && position < declared->args.size()
                        ? &decl[declared->args.get(decl, position)]->occurrence : nullptr;

                    if(!held || !isNarrowReference(layout, *held)) {
                        auto borrow = materializeNarrow(block, place, supplied->occurrence);
                        commits.push(borrow);
                        place = borrow.temporary;
                    }
                }

                passCarried(block, shape, position, place, passed);
                position++;
            }

            // And the location, where this call became the located one. After the loop because the
            // constants are the *last* arguments of the located declaration, and the written ones
            // fill the positions in front of them unchanged.
            if(located && !checkLocationArguments(block, *declared, position, inst.source, passed)) {
                return;
            }

            /*
             * An address-only result comes back through storage this frame supplies.
             *
             * The allocation is here rather than at the callee because only the caller knows where
             * the value is going to live, which is the same reason it is a *hidden argument* rather
             * than a return value: the callee writes through it and returns nothing.
             *
             * **Whose answer it is** is the callee's, and that is the other half of the boundary the
             * argument loop above adapts. A body compiled once declares its result at a type
             * parameter, so it returns through storage whatever the caller bound that to - and a
             * caller expecting an `I32` in a register still has to supply the pointer, and read the
             * register value back out of it afterwards. Reading the *caller's* manifestation here is
             * what built a call with one argument too few.
             */
            auto results = inst.results.size();
            auto manifest = results ? manifestOf(*ssa[inst.results.get(ssa, 0)]) : Manifestation();

            auto returnsInMemory = declared
                ? manifestNative(layout, declared->result).inMemory
                : results && manifest.inMemory;

            // A result the callee returns as its parts needs storage just the same, and no pointer.
            auto splitResult = shape && shape->splitsResult() && results;
            if(splitResult) returnsInMemory = false;

            auto memoryResult = results && manifest.inMemory;
            LowerPtr<LowerValue> returnPlace = nullptr;

            /*
             * `storageFor` where the result is this frame's value, so the callee writes into the
             * field, the box or the caller's own result pointer that the value is going to end up in
             * - §5.6's destination passing, decided here rather than chased afterwards.
             *
             * A plain `frameStorage` where the *callee* returns in memory and this frame wanted a
             * register: the storage is then a landing pad this walk reads back out of, and it has no
             * consumer to inherit it from.
             *
             * The storage a result returned as parts is going into is asked for here too, on the same
             * terms as a hidden pointer's: where it is a box, the allocation comes first and the parts
             * are written into it the moment the call returns, rather than held across the
             * allocator's call - which is `Tree`'s `build`, and cost it 4% when this was asked after.
             */
            LowerPtr<LowerValue> partsStorage = nullptr;
            if(splitResult) partsStorage = storageFor(block, *ssa[inst.results.get(ssa, 0)]);

            if(returnsInMemory && results) {
                returnPlace = memoryResult
                            ? storageFor(block, *ssa[inst.results.get(ssa, 0)])
                            : frameStorage(block, ssa[inst.results.get(ssa, 0)]->occurrence);
            }

            /*
             * The **callee's** convention, not this function's.
             *
             * They are the same today because every function here takes the default, and they are
             * different the moment one declaration names another - a call has to agree with what it
             * is calling, and reading the enclosing function's would agree with the caller instead.
             * Nothing would report it: the registers simply differ.
             */
            auto created = returnsInMemory ? 0u : splitResult ? shape->resultCount : U32(results);

            /*
             * The **callee's** convention where the call names one, and the default where it does
             * not - see `erase.cpp`, which builds every function reached through a slot and declares
             * that one. An indirect call has no callee to read a convention off, so what makes the
             * two ends agree is that there is a single convention on this side of the boundary.
             */
            auto direct = written.through == mir::CallThrough::Declaration;
            Maybe<LowerCallType> rawConvention = Nothing();
            if(written.through == mir::CallThrough::Value) {
                auto targetType = ssa[written.target.value()]->occurrence.type;
                if(decl[targetType]->kind == mir::Type::Fun) {
                    rawConvention = ((mir::FunType*)decl[targetType])->convention;
                }
            }
            auto convention = direct
                ? lower[functions.getValue(target.offset).unwrap()]->callType
                : rawConvention ? rawConvention.unwrap() : kDefaultCallType;

            auto leading = 1u + (environment ? 1u : 0u) + (returnPlace ? 1u : 0u);

            auto value = call(lower, to, block, created, passed.size() + leading,
                              convention, [&](LowerInstCall* built) {
                for(Size i = 0; i < created; i++) {
                    auto type = splitResult ? resultPart(*shape, U32(i)).type
                                            : manifestOf(*ssa[inst.results.get(ssa, i)]).type;
                    new (built->created().ptr + i) LowerValue(built, type, StringId());
                }

                built->used()[0] = callee;

                Size index = 1;
                if(environment) built->used()[index++] = environment;
                if(returnPlace) built->used()[index++] = returnPlace;
                for(Size i = 0; i < passed.size(); i++) built->used()[index++] = passed[i];
            });

            // What the callee wrote into a flattened reference, put back - and here because a write
            // through a reference becomes visible when the callee returns. See `PackedBorrow`.
            for(auto& borrow: commits) commitPackedBorrow(block, borrow);

            /*
             * The parts, written into the storage the value is going to live in - `storageFor`, so a
             * result bound for a field, a box or this function's own result pointer is written there
             * directly, exactly as a hidden pointer to that storage would have been.
             */
            if(splitResult) {
                value->source = inst.source;
                storeReturnedParts(block, *shape, value, partsStorage);
                break;
            }

            if(returnsInMemory && results) {
                value->source = inst.source;

                /*
                 * The storage itself where this frame wanted storage, and a load out of it where it
                 * wanted a register - `thunkFor`'s store, from the other side of the same boundary.
                 */
                // Already mapped by `storageFor`, which is what chose the storage.
                if(memoryResult) break;

                auto loaded = block.addInst(lower, new (to.arena) LowerInstLoad(
                    returnPlace, StringId(), manifest.type, manifest.size, manifest.isSigned));

                values.add(ssa[inst.results.get(ssa, 0)]->id.index,
                           ((LowerInstSingle*)loaded)->created().ptr - lower);
                break;
            }

            for(Size i = 0; i < created; i++) {
                values.add(ssa[inst.results.get(ssa, i)]->id.index, value->created().ptr + i - lower);
            }

            break;
        }
        case Inst::Return: {
            auto& written = (InstReturn&)inst;

            /*
             * An address-only result is written into the caller's storage and nothing is returned.
             *
             * The hidden pointer is this function's own leading parameter, so the copy is the same
             * `install` every other write into storage is. What the `ret` then carries is nothing,
             * which is what the signature above declared for it.
             */
            SmallArray<LowerPtr<LowerValue>, 4> returned;
            auto shape = currentShape;

            for(auto used: written.values.contents(ssa)) {
                auto value = ssa[used.value()];

                // A result this function returns as its parts - loaded out of wherever it was built.
                if(shape && shape->splitsResult() && manifestOf(*value).inMemory) {
                    auto address = mapped(used.value());
                    if(!address) return;

                    for(U32 i = 0; i < shape->resultCount; i++) {
                        returned.push(loadCarried(block, resultPart(*shape, i), address - lower));
                    }

                    continue;
                }

                if(manifestOf(*value).inMemory) {
                    // Nothing to install where the value was built in the result pointer to begin
                    // with - `storageFor`, which is what handed it that address.
                    if(destinations.getValue(value->id.index)) continue;
                    if(currentReturnPlace) install(block, currentReturnPlace, *value);
                    continue;
                }

                auto mappedValue = mapped(used.value());
                if(!mappedValue) return;
                returned.push(mappedValue - lower);
            }

            emitReturn(block, returned.pointer(), returned.size());
            break;
        }
        case Inst::Jump: {
            auto& written = (InstJump&)inst;
            auto target = blocks.getValue(ssa[written.to.target]->index.index);
            if(!target) return;

            supplyEdge(block, written.to);
            block.addInst(lower, new (to.arena) LowerInstJmp(target.unwrap()));
            break;
        }
        case Inst::Branch: {
            auto& written = (InstBranch&)inst;
            auto condition = mapped(written.condition.value());
            auto whenTrue = blocks.getValue(ssa[written.whenTrue.target]->index.index);
            auto whenFalse = blocks.getValue(ssa[written.whenFalse.target]->index.index);
            if(!condition || !whenTrue || !whenFalse) return;

            supplyEdge(block, written.whenTrue);
            supplyEdge(block, written.whenFalse);

            block.addInst(lower, new (to.arena) LowerInstJe(
                condition - lower, whenTrue.unwrap(), whenFalse.unwrap()));
            break;
        }
        /*
         * A multi-way branch, as the multi-way branch the lower IR now has - D3 slice 3.
         *
         * One instruction rather than the `je` chain this used to expand into, because the choice
         * between a chain, a binary search and a jump table is a question about the *target* and
         * nothing here knows the answer: `compiler/codegen/x64/transform_switch.cpp` decides it, and
         * `IRBuilder::CreateSwitch` hands it to LLVM to decide for itself.
         *
         * Every edge leaves the block the `match` was written in, so each target's phi takes its
         * alternative from here - which is what `supplyEdge` records. A target named by two cases
         * would then be one block with two alternatives from one predecessor, so it gets a block
         * holding a jump instead; `LowerBlock::outgoing` is where that rule is stated.
         */
        case Inst::Switch: {
            auto& written = (InstSwitch&)inst;
            auto selector = mapped(written.selector.value());
            auto fallback = blocks.getValue(ssa[written.fallback.target]->index.index);
            if(!selector || !fallback || !currentLowerFunction) return;

            auto items = written.cases.pointer(ssa);
            auto count = written.cases.size();

            auto jump = makeSwitch(to.arena, selector - lower, fallback.unwrap(), U32(count));
            auto cases = jump->cases();

            for(Size i = 0; i < count; i++) {
                auto found = blocks.getValue(ssa[items[i].edge.target]->index.index);
                if(!found) return;

                auto target = found.unwrap();
                auto from = &block;

                // A target this switch already names, or the fallback: the second edge goes through
                // a block of its own so that the pair of blocks an edge joins stays its identity.
                auto repeated = target == fallback.unwrap();
                for(Size j = 0; j < i && !repeated; j++) repeated = cases[j].target == target;

                if(repeated) {
                    auto trampoline = currentLowerFunction->addBlock(lower, StringId());
                    from = lower[trampoline - lower];
                    target = trampoline - lower;
                }

                cases[i].setValue(items[i].value);
                cases[i].target = target;

                supplyEdge(*from, items[i].edge);
                if(from != &block) from->addInst(lower, new (to.arena) LowerInstJmp(found.unwrap()));
            }

            supplyEdge(block, written.fallback);
            block.addInst(lower, (LowerInst*)jump);
            break;
        }
        case Inst::Unreachable:
            block.addInst(lower, new (to.arena) LowerInstUnreachable());
            break;

        /*
         * A host operation, which this tier never sees - Analysis-IR-JS.md §3.10's "native never
         * sees one".
         *
         * An internal error and not a translation, because there is nothing to translate *to*:
         * `@platform` selection deletes every declaration that could reach one, so a native build
         * holds no name, no type and no instance that could build a `Host` node. What this arm
         * catches is a selection bug - a `.js.yana` file read on a native build - and it catches it
         * at the one place where saying so is still cheap. JSIR is the sibling that has the arm.
         */
        case Inst::Host:
            assertTrue(false);
            break;

        case Inst::KindCount:
            break;
    }
}

} // namespace ssa
