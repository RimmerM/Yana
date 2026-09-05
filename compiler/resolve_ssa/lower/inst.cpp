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

LowerPtr<LowerValue> Lowering::frameStorage(LowerBlock& block, const mir::Occurrence& occurrence) {
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
     */
    if(!manifest.inMemory || !hasPaddedWord(layout, occurrence.type)) return storage;

    U32 at = 0;
    while(at < manifest.size) {
        auto width = manifest.size - at >= 8 ? 8u : manifest.size - at >= 4 ? 4u
                   : manifest.size - at >= 2 ? 2u : 1u;

        block.addInst(lower, new (to.arena) LowerInstStore(
            offsetAddress(block, storage, at), immediate(block, LowerType::Int64, 0), width));

        at += width;
    }

    return storage;
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

void Lowering::install(LowerBlock& block, LowerPtr<LowerValue> address, Value& value) {
    auto manifest = manifestOf(value);

    // A size of zero is `{}` and there is nothing to install. Not the same test as "no bytes": a
    // dynamic manifestation carries a size of zero that means "ask the descriptor", which is the
    // one case reading `size` directly here would get wrong.
    if(!manifest.dynamic && !manifest.size) return;

    auto source = mapped(&value - ssa);
    if(!source) return;

    if(!manifest.inMemory) {
        block.addInst(lower, new (to.arena) LowerInstStore(address, source - lower, manifest.size));
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

void Lowering::emitTeardown(LowerBlock& block, LowerPtr<LowerValue> address, DeclPtr<mir::Type> type,
                            bool runDrop, bool runReclaim, LocationId source, BoxRelease boxes) {
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

        auto convention = lower[found.unwrap()]->callType;
        call(lower, to, block, 0, 2, convention, [&](LowerInstCall* built) {
            built->used()[0] = ((LowerInstSingle*)callee)->created().ptr - lower;
            built->used()[1] = address;
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
    if(!childDrop && !childReclaim) return;

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
        if(!array.hasFixedLength() || !mir::hasTeardown(decl, array.element)) return;

        auto stride = elementStrideNative(layout, array.element);

        for(U64 i = 0; i < array.length; i++) {
            emitTeardown(block, offsetAddress(block, address, U32(stride * i)), array.element,
                         childDrop, childReclaim, source, boxes);
        }

        return;
    }

    /*
     * A **function value** never reaches here, and `ownershipOf` is where the reason is written: an
     * environment holds copies and borrows, so a closure owes nothing. What a release would have had
     * to read - a word in front of the lifted function's entry naming its release routine - is a
     * word nothing writes, and it arrives with the heap placement that makes an environment able to
     * own something in the first place.
     */
    if(decl[type]->kind != mir::Type::Record) return;

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
     */
    if(record->layout != mir::RecordLayout::Single) {
        auto callee = teardownHalves(type, childDrop, childReclaim, false, source, boxes);

        auto name = block.addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));
        call(lower, to, block, 0, 2, lower[callee]->callType, [&](LowerInstCall* built) {
            built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
            built->used()[1] = address;
        });

        return;
    }

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
            emitBoxTeardown(block, address, type, field.index, nullptr, childDrop, childReclaim,
                            childReclaim ? boxes : BoxRelease::Foreign, source);
            continue;
        }

        emitTeardown(block, fieldAddress(block, address, type, field.index), field.occurrence.type,
                     childDrop, childReclaim, source, boxes);
    }
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
    auto tagBits = mir::tagBitsOf(decl, record);
    auto tagLower = tagBits > 32 ? LowerType::Int64 : LowerType::Int32;
    auto repr = reprOf(layout, type);

    /*
     * Which constructor is in there, however this record spells that - a load of the tag word, or
     * the range test a folded one decodes to. Asked of the layout rather than assumed, on the same
     * terms every other reader of a discriminant asks it.
     */
    LowerPtr<LowerValue> tag = nullptr;

    if(repr.isFolded()) {
        VirtualPlace place;
        place.kind = VirtualPlace::Folded;
        place.word = address;
        place.record = type;

        tag = decodeFolded(entry, place, tagLower);
    } else if(repr.isBitTagged()) {
        tag = decodePacked(entry, discriminantPlace(entry, address, repr), tagLower);
    } else {
        tag = wordAt(entry, address, repr.discriminantOffset, tagLower, repr.discriminantBytes);
    }

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
                emitBoxTeardown(*arm, address, type, field.index, nullptr, runDrop, runReclaim,
                                runReclaim ? boxes : BoxRelease::Foreign, source);
                continue;
            }

            emitTeardown(*arm, fieldAddress(*arm, address, type, field.index),
                         field.occurrence.type, runDrop, runReclaim, source, boxes);
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

void Lowering::lowerInst(LowerBlock& block, Inst& inst) {
    switch(inst.kind) {
        case Inst::Const: {
            auto& written = (InstConst&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = manifestOf(*result);

            /*
             * A constant with **no register form** - a record, which is storage.
             *
             * What a value of a memory type is is the address of its bytes, so a constant of one has
             * to be placed somewhere before it can be named; `placeConstant` is where that decision
             * is, and what comes back here is an address like any other. A `let` of it then copies
             * from that address exactly as it copies from any other record.
             */
            if(manifest.inMemory) {
                auto placed = placeConstant(written.constant, result->occurrence);
                auto address = block.addInst(lower, new (to.arena) LowerInstGlobal(
                    lower[placed]->name, placed));

                values.add(result->id.index, &((LowerInstSingle*)address)->result - lower);
                break;
            }

            LowerInst* value;
            if(written.constant.kind == mir::Constant::Float) {
                value = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), manifest.type, written.constant.asFloat()));
            } else {
                // `constantBits`, not `bits` - an enum constant names a constructor by index and
                // the number it stores is the declaration's. See the note on the declaration.
                value = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), manifest.type, constantBits(decl, written.constant)));
            }

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }
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
            values.add(result->id.index, frameStorage(block, written.held));
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

            install(block, address - lower, *ssa[written.value.value()]);

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
                 */
                auto& target = *decl[result->type()];
                auto signedSource = logical.kind == mir::Type::Int && ((mir::IntType&)logical).isSigned;
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
                 * ever generated, `intrinsic::reinterpretable` - so nothing here truncates and there
                 * is no extension for a sign to decide.
                 */
                value = block.addInst(lower, new (to.arena) LowerInstUnary(
                    LowerInst::Bitcast, StringId(), manifest.type, operand - lower));
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

                auto built = (LowerInstIntrinsic*)to.arena.alloc(
                    sizeof(LowerInstIntrinsic) + sizeof(LowerValue) + sizeof(LowerPtr<LowerValue>));

                new (built) LowerInstIntrinsic(which, 1, 1);
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

            if(width && wrapsAtDeclaredWidth(mir::unaryWidthEffect(written.op))) {
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

            if(isNarrowReference(layout, from.occurrence) &&
               !isReferenceValue(layout, result->occurrence)) {
                spendPlace(block, *result,
                           unpackNarrowRef(block, address - lower, from.occurrence.type),
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
                address - lower, StringId(), manifest.type, manifest.size, manifest.isSigned));

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

            auto storage = frameStorage(block, result->occurrence);

            block.addInst(lower, new (to.arena) LowerInstCopy(
                storage, address - lower, sizeValue(block, result->occurrence)));

            values.add(result->id.index, storage);
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

            auto storage = frameStorage(block, result->occurrence);

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

                U32 offset = 0;
                for(auto used: written.components.contents(ssa)) {
                    install(block, offsetAddress(block, storage, offset), *ssa[used.value()]);
                    offset += stride;
                }

                values.add(result->id.index, storage);
                break;
            }

            auto& declared = *(mir::RecordType*)decl[record];
            auto arm = declared.cons.get(decl, written.constructor);
            auto repr = reprOf(layout, record, result->occurrence.shape);

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

                install(block, offsetAddress(block, storage, held.offset), *ssa[used.value()]);
            }

            values.add(result->id.index, storage);
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

            auto memory = to.arena.alloc(sizeof(LowerInstVecShuffle) +
                                         LowerInstVecShuffle::patternBytes(type));
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
                case mir::ReduceOp::Count: assertTrue(false); break;
            }

            auto folded = block.addInst(lower, new (to.arena) LowerInstVecReduce(
                StringId(), manifest.type, from - lower, reduce));

            auto value = &((LowerInstSingle*)folded)->result - lower;

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
                        lowerOrder(written.order)));

                    values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                    break;
                }

                case mir::AtomicOp::Store: {
                    auto manifest = accessOf(*ssa[written.arguments.get(ssa, 1).value()]);

                    block.addInst(lower, new (to.arena) LowerInstAtomicStore(
                        args[0], args[1], manifest.size, lowerOrder(written.order)));
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
                    values.add(previous->id.index, ((LowerInstAtomicCas*)cas)->created().ptr - lower);
                    break;
                }

                default: {
                    // The read-modify-writes, which are one shape - `atomicRmwOp` is what says which
                    // arithmetic, and it is total over exactly this group.
                    auto result = ssa[inst.results.get(ssa, 0)];
                    auto manifest = accessOf(*result);

                    auto value = block.addInst(lower, new (to.arena) LowerInstAtomicRmw(
                        args[0], args[1], StringId(), manifest.type, manifest.size,
                        atomicRmwOp(written.op), lowerOrder(written.order)));

                    values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
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

            switch(written.through) {
                case mir::CallThrough::Declaration: {
                    auto found = functions.getValue(written.callee.offset);
                    if(!found) {
                        context.diagnostics.error(
                            "internal: a call reached a callee that was not lowered"_v, inst.source);
                        return;
                    }

                    callee = ((LowerInstSingle*)block.addInst(lower, new (to.arena) LowerInstFun(
                        decl[written.callee]->name, found.unwrap())))->created().ptr - lower;

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

                    callee = wordAt(block, pair - lower, layout.target.funCodeOffset(),
                                    LowerType::Pointer, layout.target.pointerSize);
                    environment = wordAt(block, pair - lower, layout.target.funEnvOffset(),
                                         LowerType::Pointer, layout.target.pointerSize);
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
                ? decl[written.callee] : nullptr;

            SmallArray<LowerPtr<LowerValue>, 8> passed;
            SmallArray<PackedBorrow, 4> commits;
            U32 position = 0;

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

                passed.push(place);
                position++;
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

            auto memoryResult = results && manifest.inMemory;
            LowerPtr<LowerValue> returnPlace = nullptr;

            if(returnsInMemory && results) {
                returnPlace = frameStorage(block, ssa[inst.results.get(ssa, 0)]->occurrence);
            }

            /*
             * The **callee's** convention, not this function's.
             *
             * They are the same today because every function here takes the default, and they are
             * different the moment one declaration names another - a call has to agree with what it
             * is calling, and reading the enclosing function's would agree with the caller instead.
             * Nothing would report it: the registers simply differ.
             */
            auto created = returnsInMemory ? 0u : U32(results);

            /*
             * The **callee's** convention where the call names one, and the default where it does
             * not - see `erase.cpp`, which builds every function reached through a slot and declares
             * that one. An indirect call has no callee to read a convention off, so what makes the
             * two ends agree is that there is a single convention on this side of the boundary.
             */
            auto direct = written.through == mir::CallThrough::Declaration;
            auto convention = direct
                ? lower[functions.getValue(written.callee.offset).unwrap()]->callType
                : kDefaultCallType;

            auto leading = 1u + (environment ? 1u : 0u) + (returnPlace ? 1u : 0u);

            auto value = call(lower, to, block, created, passed.size() + leading,
                              convention, [&](LowerInstCall* built) {
                for(Size i = 0; i < created; i++) {
                    auto result = ssa[inst.results.get(ssa, i)];
                    new (built->created().ptr + i) LowerValue(built, manifestOf(*result).type,
                                                              StringId());
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

            if(returnsInMemory && results) {
                value->source = inst.source;

                /*
                 * The storage itself where this frame wanted storage, and a load out of it where it
                 * wanted a register - `thunkFor`'s store, from the other side of the same boundary.
                 */
                if(memoryResult) {
                    values.add(ssa[inst.results.get(ssa, 0)]->id.index, returnPlace);
                    break;
                }

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
            for(auto used: written.values.contents(ssa)) {
                auto value = ssa[used.value()];
                if(manifestOf(*value).inMemory) {
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
        case Inst::Unreachable:
            block.addInst(lower, new (to.arena) LowerInstUnreachable());
            break;
        case Inst::KindCount:
            break;
    }
}

} // namespace ssa
