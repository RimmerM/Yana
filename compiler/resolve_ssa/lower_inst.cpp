#include "lower_internal.h"
#include "../resolve_mir/specialize.h"

namespace ssa {

namespace {

/*
 * Which machine operation an SSA operation over this type becomes.
 *
 * The one place signedness enters the pipeline, which is why op.def has no column for it: `I32` and
 * `U32` are two logical types over one `div`, and it is here - with the type in hand and the target
 * decided - that the two become `idiv` and `div`. The same reading makes `<` two condition codes and
 * `+` one instruction.
 */
static LowerInst::Kind binaryKind(DeclBase decl, mir::BinaryOp op, DeclPtr<mir::Type> operandType) {
    auto& logical = *decl[operandType];
    auto floating = logical.kind == mir::Type::Float;
    auto isSigned = logical.kind == mir::Type::Int && ((mir::IntType&)logical).isSigned;

    switch(op) {
        case mir::BinaryOp::Add: return LowerInst::Add;
        case mir::BinaryOp::Sub: return LowerInst::Sub;
        case mir::BinaryOp::Mul: return floating || !isSigned ? LowerInst::Mul : LowerInst::IMul;
        case mir::BinaryOp::Div: return floating || !isSigned ? LowerInst::Div : LowerInst::IDiv;
        case mir::BinaryOp::Rem: return isSigned ? LowerInst::IRem : LowerInst::Rem;
        case mir::BinaryOp::And: return LowerInst::And;
        case mir::BinaryOp::Or:  return LowerInst::Or;
        case mir::BinaryOp::Xor: return LowerInst::Xor;
        case mir::BinaryOp::Shl: return LowerInst::Shl;
        case mir::BinaryOp::Shr: return LowerInst::Shr;
        case mir::BinaryOp::Sar: return LowerInst::Sar;

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
    auto& logical = *decl[operandType];
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

    return ((LowerInstSingle*)value)->created().ptr - lower;
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

LowerPtr<LowerValue> Lowering::fieldAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                            DeclPtr<mir::Type> record, U16 field) {
    return offsetAddress(block, address, fieldOffsetNative(layout, record, field));
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
                            bool runDrop, bool runReclaim, LocationId source) {
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
        auto callee = teardownHalves(type, childDrop, childReclaim, false, source);

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

        emitTeardown(block, fieldAddress(block, address, type, field.index), field.occurrence.type,
                     childDrop, childReclaim, source);
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
                                      bool runDrop, bool runReclaim, LocationId source) {
    auto& record = *(mir::RecordType*)decl[type];
    auto tagBits = mir::tagBitsOf(decl, record);

    auto tag = wordAt(entry, address, discriminantOffsetNative(layout, type),
                      tagBits > 32 ? LowerType::Int64 : LowerType::Int32, tagBits / 8);

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
            StringId(), tagBits > 32 ? LowerType::Int64 : LowerType::Int32, U64(constructor.value)));

        auto equal = current->addInst(lower, new (to.arena) LowerInstCmp(
            StringId(), tag, ((LowerInstSingle*)number)->created().ptr - lower, LowerCmp::eq));

        current->addInst(lower, new (to.arena) LowerInstJe(
            ((LowerInstSingle*)equal)->created().ptr - lower, arm - lower, next - lower));

        for(U16 i = constructor.first; i < U16(constructor.first + constructor.count); i++) {
            auto field = record.fields.get(decl, i);
            if(!mir::tearsDownField(decl, field)) continue;

            emitTeardown(*arm, fieldAddress(*arm, address, type, field.index),
                         field.occurrence.type, runDrop, runReclaim, source);
        }

        arm->addInst(lower, new (to.arena) LowerInstJmp(done - lower));
        current = next;
    }

    current->addInst(lower, new (to.arena) LowerInstJmp(done - lower));
    return done;
}

void Lowering::lowerInst(LowerBlock& block, Inst& inst) {
    switch(inst.kind) {
        case Inst::Const: {
            auto& written = (InstConst&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = manifestOf(*result);

            LowerInst* value;
            if(written.constant.kind == mir::Constant::Float) {
                value = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), manifest.type, written.constant.asFloat()));
            } else {
                value = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), manifest.type, written.constant.bits));
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
             * The slot's top byte is the field's bit shift and is not masked off here, because it is
             * provably zero wherever it is not read: `shiftOfField` writes one only for a field that
             * does not fill its storage, and such a field is refused until the load-shift-mask half
             * of this exists. See `kPropertyShiftShift`, where the pair is argued.
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

                auto place = readProperty(block, address - lower, slot, held, form);
                values.add(result->id.index, place);

                // Only the two shapes that copied have anything to put back - see `PropertyAccess`.
                if(form != PropertyForm::Offset) {
                    propertyAccesses.add(result->id.index, PropertyAccess {
                        address - lower, place, slot, form, held });
                }

                break;
            }

            /*
             * The discriminant - the same `add` over the offset the layout gave it, which is the
             * whole of what `kDiscriminant` means. What is loaded from it is the tag's own integer
             * type, so the load's width comes off the result's occurrence like every other.
             */
            if(written.field == InstFieldAddress::kDiscriminant) {
                values.add(result->id.index,
                           offsetAddress(block, address - lower,
                                         discriminantOffsetNative(layout, written.record)));
                break;
            }

            values.add(result->id.index,
                       fieldAddress(block, address - lower, written.record, written.field));
            break;
        }
        case Inst::Store: {
            auto& written = (InstStore&)inst;
            auto address = mapped(written.address.value());
            if(!address) return;

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

            LowerInst* value;
            if(kind == LowerInst::Cmp) {
                value = block.addInst(lower, new (to.arena) LowerInstCmp(
                    StringId(), lhs - lower, rhs - lower,
                    lowerCmp(decl, written.op, operandType)));
            } else {
                auto manifest = manifestOf(*result);
                value = block.addInst(lower, new (to.arena) LowerInstBinary(
                    StringId(), manifest.type, lhs - lower, rhs - lower, kind));
            }

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }
        case Inst::Unary: {
            auto& written = (InstUnary&)inst;
            auto operand = mapped(written.value.value());
            if(!operand) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = manifestOf(*result);

            LowerInst* value;
            if(written.op == mir::UnaryOp::Not) {
                /*
                 * `!b` is one exclusive or, not a complement.
                 *
                 * A `Bool` is one byte holding 0 or 1 in a 32-bit register, so inverting every bit
                 * of it answers 254 rather than False. That the two are different is exactly why
                 * `Bool` is not a one-bit integer up above: the operation is logical, and this is
                 * the file that knows what a `Bool` looks like.
                 */
                auto one = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), manifest.type, U64(1)));

                value = block.addInst(lower, new (to.arena) LowerInstBinary(
                    StringId(), manifest.type, operand - lower,
                    ((LowerInstSingle*)one)->created().ptr - lower, LowerInst::Xor));
            } else {
                value = block.addInst(lower, new (to.arena) LowerInstUnary(
                    LowerInst::Neg, StringId(), manifest.type, operand - lower));
            }

            values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
            break;
        }
        case Inst::Load: {
            auto& written = (InstLoad&)inst;
            auto address = mapped(written.address.value());
            if(!address) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = manifestOf(*result);

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
            auto& declared = *(mir::RecordType*)decl[record];
            auto arm = declared.cons.get(decl, written.constructor);

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
                auto value = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), manifest.type, U64(arm.value)));

                values.add(result->id.index, ((LowerInstSingle*)value)->created().ptr - lower);
                break;
            }

            auto storage = frameStorage(block, result->occurrence);

            /*
             * The discriminant first, where the record has one.
             *
             * A plain store of a literal, because which constructor is being built is known here:
             * this is the only thing that writes a tag, and everything that reads one reads it back
             * through the same offset. `layoutRecord` is where the offset is decided and says why it
             * is zero.
             */
            if(auto tagBits = mir::tagBitsOf(decl, declared)) {
                auto tag = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), tagBits > 32 ? LowerType::Int64 : LowerType::Int32,
                    U64(arm.value)));

                block.addInst(lower, new (to.arena) LowerInstStore(
                    storage, ((LowerInstSingle*)tag)->created().ptr - lower, tagBits / 8));
            }

            // One field at a time, into the storage the record occupies. `install` is what decides
            // whether a component is a store or a block copy, which is the same decision every other
            // write into storage makes. The components are this constructor's fields, in its order,
            // which is what `fieldAddress` turns into the arm's own offsets.
            U16 index = arm.first;

            for(auto used: written.components.contents(ssa)) {
                install(block, fieldAddress(block, storage, record, index), *ssa[used.value()]);
                index++;
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

            emitTeardown(block, address - lower, written.held.type, true, true, inst.source);
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

                    callee = wordAt(block, pair - lower, kFunCodeOffset, LowerType::Pointer,
                                    kPointerSize);
                    environment = wordAt(block, pair - lower, kFunEnvOffset, LowerType::Pointer,
                                         kPointerSize);
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
