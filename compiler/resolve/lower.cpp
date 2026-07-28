#include "lower.h"
#include "analyze.h"
#include "../lower/lower_builder.h"

// The side tables mapping one IR to the other are keyed by region offset rather than by address:
// a resolve handle already is that offset, so this is the same identity the rest of the resolver
// uses, and it stays meaningful in printed output.
struct LowerContext {
    Context& context;
    Program& from;
    LowerModule& to;
    GlobalBase global;
    ModuleBase local;
    LowerBase lower;
    HashMap<U32, LowerPtr<LowerFunction>> functions;
    HashMap<U32, LowerPtr<LowerBlock>> blocks;
    HashMap<U32, LowerPtr<LowerValue>> values;
    HashMap<U32, LowerPtr<LowerValue>> returnPlaces;
    LowerBlock* constantBlock = nullptr;

    // The fields of each scalarized local of the function being lowered, by local index and then
    // by field index. Empty for every local that kept its storage - see prepareScalars().
    Array<Array<LowerPtr<LowerValue>>> scalars;
};

/*
 * Aggregates that evaporate.
 *
 * An owner whose representation has to be writable, addressable or resizable needs storage; one
 * that needs none of those is a value, and the fields it was built from can stay in registers. That
 * is the whole of what a Repr *variant* is at this milestone - there is no packing and no niche
 * yet, so the only two representations that differ are "in memory" and "not in memory at all".
 *
 * Which is why the demand analysis of Implementation-IR.md part 5 is what decides it rather than
 * anything lowering could work out for itself: read-only is a fact about every use of an owner
 * across the whole function, and `needsStableAddress` is the one that would otherwise be found out
 * too late, after the address had already been handed to someone.
 *
 * The rest of the conditions are lowering's own, and they are about what this translation can
 * express rather than about what is true:
 *
 *  - one basic block, because a field that flowed through a branch would need a phi per field, and
 *    building those is a real pass rather than a special case of this one;
 *  - a field path of exactly one Field projection, since a nested aggregate would have to be
 *    exploded recursively;
 *  - nothing that names the aggregate as a whole - passing it to a call, returning it, dropping it -
 *    because there is no whole left to name.
 */
static bool scalarizable(LowerContext& lower, Function& function, U32 index, OwnershipResult& ownership) {
    if(index >= ownership.locals.size()) return false;

    auto& tracked = ownership.locals[index];
    auto slot = function.localAt(lower.local, index);

    if(tracked.requirements.mutation != MutationDemand::ReadOnly) return false;
    if(tracked.requirements.needsStableAddress || tracked.requirements.mayResize) return false;
    if(tracked.escapes || tracked.droppable || slot.storage != StorageClass::Stack) return false;
    if(!slot.value || lower.local[slot.value]->kind != Value::Alloc) return false;
    if(!isMemoryType(lower.global, slot.type)) return false;

    // Only a shape whose fields are a flat list, which is what a single Field projection can name.
    auto content = slot.type;
    if(lower.global[content]->kind == Type::Record) {
        auto record = (RecordType*)lower.global[content];
        if(record->layout != RecordType::Single || record->constructors.isEmpty()) return false;

        content = record->constructors.get(lower.global, 0).content;
    }

    if(!content || lower.global[content]->kind != Type::Tup) return false;

    for(auto field: ((TupType*)lower.global[content])->fields.contents(lower.global)) {
        if(isMemoryType(lower.global, field.type)) return false;
    }

    // Every use has to be one this translation can rewrite, and all of them in one block.
    ModulePtr<Block> only = nullptr;

    for(auto user: lower.local[slot.value]->uses.contents(lower.local)) {
        auto& instruction = *lower.local[user];

        if(!only) only = instruction.block;
        else if(only != instruction.block) return false;

        Place place;

        switch(instruction.kind) {
            case Value::Init: place = ((InstInit&)instruction).place; break;
            case Value::LoadPlace: place = ((InstLoadPlace&)instruction).place; break;
            default: return false;
        }

        if(place.root != PlaceRoot::Local || place.local != index) return false;

        auto path = place.projections;
        auto steps = path.contents(lower.local);
        auto fields = 0u;

        for(auto projection: steps) {
            if(projection.kind == ProjectionKind::Downcast) continue;
            if(projection.kind != ProjectionKind::Field) return false;
            fields++;
        }

        if(fields != 1) return false;
    }

    return only != nullptr;
}

// The field one place names, for a local this pass decided to scalarize. Only ever asked of a
// place scalarizable() already accepted, so the path is known to be one Field.
static U16 scalarField(LowerContext& lower, const Place& place) {
    auto path = place.projections;

    for(auto projection: path.contents(lower.local)) {
        if(projection.kind == ProjectionKind::Field) return projection.index;
    }

    return 0;
}

static bool isScalarPlace(LowerContext& lower, const Place& place) {
    return place.root == PlaceRoot::Local && place.local < lower.scalars.size() &&
           lower.scalars[place.local].size() > 0;
}

static void prepareScalars(LowerContext& lower, ModulePtr<Function> pointer, Function& function) {
    lower.scalars.clear();
    for(Size i = 0; i < function.localCount(); i++) lower.scalars.push(Array<LowerPtr<LowerValue>>());

    if(!lower.from.ownership) return;

    auto found = lower.from.ownership->functions.get(U32(pointer));
    if(!found) return;

    auto& ownership = found.unwrap();

    for(U32 i = 0; i < function.localCount(); i++) {
        if(!scalarizable(lower, function, i, ownership)) continue;

        auto content = function.localAt(lower.local, i).type;
        if(lower.global[content]->kind == Type::Record) {
            content = ((RecordType*)lower.global[content])->constructors.get(lower.global, 0).content;
        }

        auto count = ((TupType*)lower.global[content])->fields.size();
        for(Size f = 0; f < count; f++) lower.scalars[i].push(nullptr);
    }
}

static LowerType lowerType(GlobalBase base, TypePtr type) {
    auto value = base[type];
    if(value->kind == Type::Record && ((RecordType*)value)->layout == RecordType::Enum) {
        return LowerType::Int32;
    }

    if(value->kind == Type::Ptr || value->kind == Type::Borrow || isMemoryType(base, type)) {
        return LowerType::Pointer;
    }

    if(value->kind == Type::Int) {
        return ((IntType*)value)->width == IntType::Long ? LowerType::Int64 : LowerType::Int32;
    }

    if(value->kind == Type::Float) {
        return ((FloatType*)value)->width == FloatType::Double ? LowerType::Float64 : LowerType::Float32;
    }

    assertTrue("unit and unsupported types have no lower value" == nullptr);
    return LowerType::Int32;
}

static bool signedType(GlobalBase base, TypePtr type) {
    return base[type]->kind == Type::Int && ((IntType*)base[type])->isSigned;
}

static U32 memoryWidth(GlobalBase base, TypePtr type) {
    auto size = typeSize(base, type);
    assertTrue(size == 1 || size == 2 || size == 4 || size == 8);
    return size;
}

static LowerPtr<LowerValue> immediate(LowerContext& lower, U64 value, LowerType type = LowerType::Int64) {
    auto instruction = new (lower.to.arena) LowerImm(0, type, value);
    lower.constantBlock->addInst(lower.lower, instruction);
    return instruction->created().ptr - lower.lower;
}

static LowerPtr<LowerValue> mappedValue(LowerContext& lower, ModulePtr<Value> pointer);

// Folds an accumulated constant offset into an address, which is what every projection path comes
// down to once the aggregate structure is gone.
static LowerPtr<LowerValue> addOffset(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> address, U32 offset) {
    if(!offset) return address;

    auto offsetValue = immediate(lower, offset);
    auto add = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[address], lower.lower[offsetValue], LowerType::Pointer, 0);
    return add->created().ptr - lower.lower;
}

// A place becomes the address of whatever it is rooted in plus the constant offset its
// projections add up to. Nothing else survives: the lower IR has no aggregates, so this is where
// field access stops being structural and becomes arithmetic.
//
// The three roots differ only in where that first address comes from - a local's alloca, a
// global's static address, or a pointer the program computed - which is exactly why raw memory
// needs no lowering of its own beyond a root the resolver was already able to name.
static LowerPtr<LowerValue> lowerPlace(LowerContext& lower, LowerBlock& block, Function& function, const Place& place) {
    LowerPtr<LowerValue> address;
    TypePtr type;

    if(place.root == PlaceRoot::Global) {
        auto global_ = lower.local[place.global];
        auto target = lower.to.globals.getValue(global_->name).unwrap();
        auto load = block.addInst(lower.lower, new (lower.to.arena) LowerInstGlobal(global_->name, target));

        address = load->created().ptr - lower.lower;
        type = global_->type;
    } else if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        // A borrow is an address once the checking is done, so the two roots lower alike; all
        // that differed between them was how much could be proved before reaching here.
        address = mappedValue(lower, place.pointer);
        auto referenced = lower.local[place.pointer]->type;

        type = place.root == PlaceRoot::Borrow
            ? ((BorrowType*)lower.global[referenced])->to
            : pointeeType(lower.global, referenced);
    } else {
        assertTrue(place.local < function.localCount());

        auto root = function.localAt(lower.local, place.local);
        address = mappedValue(lower, root.value);
        type = root.type;
    }

    U32 offset = 0;
    auto projections = place.projections;

    for(auto projection: projections.contents(lower.local)) {
        if(projection.kind == ProjectionKind::Discriminant) {
            type = lower.from.scalar.int_;
        } else if(projection.kind == ProjectionKind::Downcast) {
            auto record = (RecordType*)lower.global[type];
            if(record->layout == RecordType::Multi) offset += record->payloadOffset;
            type = record->constructors.get(lower.global, projection.index).content;
        } else if(projection.kind == ProjectionKind::Field) {
            auto tuple = (TupType*)lower.global[type];
            auto field = tuple->fields.get(lower.global, projection.index);
            offset += field.offset;
            type = field.type;
        } else if(projection.kind == ProjectionKind::Deref) {
            // The pointer stored here becomes the address the rest of the path is relative to,
            // so everything accumulated so far has to be spent before it is loaded.
            auto from = addOffset(lower, block, address, offset);
            auto loaded = load(lower.lower, lower.to, block, lower.lower[from], 8, false, LowerType::Pointer, 0);

            address = loaded->created().ptr - lower.lower;
            type = pointeeType(lower.global, type);
            offset = 0;
        } else {
            assertTrue("unsupported place projection reached lowering" == nullptr);
        }
    }

    return addOffset(lower, block, address, offset);
}

// Constants belong to no block in the resolve IR, so each one is materialized once per function
// in the entry block the first time something asks for it.
static LowerPtr<LowerValue> mapConstant(LowerContext& lower, ModulePtr<Value> pointer) {
    auto& value = *lower.local[pointer];
    LowerInst* instruction;
    auto type = lowerType(lower.global, value.type);

    switch(value.kind) {
        case Value::ConstInt:
            instruction = new (lower.to.arena) LowerImm(value.name, type, ((ConstInt&)value).value);
            break;
        case Value::ConstFloat:
            instruction = new (lower.to.arena) LowerImm(value.name, type, F64(((ConstFloat&)value).value));
            break;
        case Value::ConstDouble:
            instruction = new (lower.to.arena) LowerImm(value.name, type, ((ConstDouble&)value).value);
            break;
        default:
            assertTrue("expected constant" == nullptr);
            return nullptr;
    }

    instruction->source = value.source;
    lower.constantBlock->addInst(lower.lower, instruction);

    auto result = instruction->created().ptr - lower.lower;
    lower.values.add(pointer, result);
    return result;
}

static LowerPtr<LowerValue> mappedValue(LowerContext& lower, ModulePtr<Value> pointer) {
    if(!pointer) return nullptr;
    if(auto found = lower.values.get(pointer)) return found.unwrap();
    if(isConstant(*lower.local[pointer])) return mapConstant(lower, pointer);

    assertTrue("resolve value was used before it was lowered" == nullptr);
    return nullptr;
}

static LowerCmp lowerCmp(LowerContext& lower, InstCmp& compare) {
    auto signedOperands = signedType(lower.global, lower.local[compare.lhs]->type);

    switch(compare.cmp) {
        case CompareOp::Eq: return LowerCmp::eq;
        case CompareOp::Ne: return LowerCmp::neq;
        case CompareOp::Gt: return signedOperands ? LowerCmp::igt : LowerCmp::gt;
        case CompareOp::Ge: return signedOperands ? LowerCmp::ige : LowerCmp::ge;
        case CompareOp::Lt: return signedOperands ? LowerCmp::ilt : LowerCmp::lt;
        case CompareOp::Le: return signedOperands ? LowerCmp::ile : LowerCmp::le;
    }

    return LowerCmp::eq;
}

static void mapResult(LowerContext& lower, ModulePtr<Value> from, LowerInst* instruction) {
    auto& value = *lower.local[from];
    instruction->source = value.source;

    if(!isUnit(lower.global, value.type)) {
        assertTrue(instruction->createdCount == 1);
        lower.values.add(from, instruction->created().ptr - lower.lower);
    }
}

static LowerInst::Kind binaryKind(LowerContext& lower, InstBinary& binary) {
    auto floating = isFloat(lower.global, binary.type);

    // Which of the two multiply/divide/remainder instructions an integer operation becomes is the
    // type's own signedness: an unsigned type's arithmetic is the unsigned one, which is the
    // whole of what makes Native's U8..U64 different from the I-family at the machine level.
    auto signed_ = signedType(lower.global, binary.type);

    switch(binary.kind) {
        case Value::Add: return LowerInst::Add;
        case Value::Sub: return LowerInst::Sub;
        case Value::Mul: return floating ? LowerInst::Mul : (signed_ ? LowerInst::IMul : LowerInst::Mul);
        case Value::Div: return floating ? LowerInst::Div : (signed_ ? LowerInst::IDiv : LowerInst::Div);
        case Value::Rem: return signed_ ? LowerInst::IRem : LowerInst::Rem;
        case Value::Shl: return LowerInst::Shl;
        case Value::Shr: return LowerInst::Shr;
        case Value::Sar: return LowerInst::Sar;
        case Value::And: return LowerInst::And;
        case Value::Or: return LowerInst::Or;
        case Value::Xor: return LowerInst::Xor;
        default:
            assertTrue("expected binary instruction" == nullptr);
            return LowerInst::Add;
    }
}

static void lowerInstruction(LowerContext& lower, LowerBlock& block, ModulePtr<Inst> pointer) {
    auto& instruction = *lower.local[pointer];
    auto instValue = (ModulePtr<Value>)pointer;
    LowerInst* result = nullptr;
    auto function = lower.local[lower.local[instruction.block]->function];

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
                return;
            }

            auto bytes = immediate(lower, typeSize(lower.global, instruction.type));

            if(allocation.storage == StorageClass::Heap) {
                // Storage escape analysis proved the frame cannot hold, so it comes from the
                // Native allocator instead. The release is an InstDrop the drop pass inserted,
                // or the new owner's - see InstAlloc::releasedHere.
                auto target = lower.functions.getValue(lower.from.allocateHeap).unwrap();
                auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, target));

                result = call(lower.lower, lower.to, block, 1, 2, lower.lower[target]->callType,
                              [&](LowerInstCall* allocate) {
                    new (allocate->created().ptr) LowerValue(allocate, LowerType::Pointer, instruction.name);
                    allocate->used()[0] = fun->created().ptr - lower.lower;
                    allocate->used()[1] = bytes;
                });

                result->source = instruction.source;
                lower.values.add(instValue, result->created().ptr - lower.lower);
                return;
            }

            assertTrue(allocation.storage == StorageClass::Stack);
            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, typeAlign(lower.global, instruction.type)));
            break;
        }
        case Value::LoadPlace: {
            auto& loadInst = (InstLoadPlace&)instruction;

            // Reading a field of a scalarized aggregate is naming the value that was written into
            // it, which is what makes the load disappear rather than become a cheaper load.
            if(isScalarPlace(lower, loadInst.place)) {
                lower.values.add(instValue, lower.scalars[loadInst.place.local][scalarField(lower, loadInst.place)]);
                return;
            }

            auto address = lowerPlace(lower, block, *function, loadInst.place);

            // An aggregate is never loaded into a value: the address of its storage is what the
            // rest of the lowering uses in its place.
            if(isMemoryType(lower.global, instruction.type)) {
                lower.values.add(instValue, address);
                return;
            }

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                memoryWidth(lower.global, instruction.type),
                signedType(lower.global, instruction.type),
                lowerType(lower.global, instruction.type),
                instruction.name
            );
            break;
        }
        case Value::Init:
        case Value::Assign: {
            // The two are one instruction here. Whatever the old value's drop needed has already
            // been emitted as its own InstDrop by the drop pass, so by the time lowering sees an
            // assignment there is nothing left in it but the write.
            auto& init = (InstInit&)instruction;

            if(isScalarPlace(lower, init.place)) {
                lower.scalars[init.place.local][scalarField(lower, init.place)] = mappedValue(lower, init.value);
                return;
            }

            auto address = lowerPlace(lower, block, *function, init.place);
            auto value = mappedValue(lower, init.value);

            if(isMemoryType(lower.global, lower.local[init.value]->type)) {
                auto count = immediate(lower, typeSize(lower.global, lower.local[init.value]->type));
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(address, value, count));
            } else {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, value, memoryWidth(lower.global, lower.local[init.value]->type)));
            }

            break;
        }
        case Value::Borrow: {
            // A borrow is the address of what it borrows. Nothing is loaded and nothing is copied,
            // which is the whole of what "non-owning, zero-cost" means once the checking is done.
            auto address = lowerPlace(lower, block, *function, ((InstBorrow&)instruction).place);
            lower.values.add(instValue, address);
            return;
        }
        case Value::Move: {
            // A bitwise move needs no code at all: the bytes stay where they are and what changed
            // is only which name is allowed to reach them. An authored Sink is the call the
            // resolver already attached, and is emitted by the drop pass rather than here, so a
            // move reaching lowering is always the bitwise one.
            auto& moved = (InstMove&)instruction;
            assertTrue(moved.sink == nullptr);

            auto address = lowerPlace(lower, block, *function, moved.place);

            if(isMemoryType(lower.global, instruction.type)) {
                lower.values.add(instValue, address);
                return;
            }

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                memoryWidth(lower.global, instruction.type),
                signedType(lower.global, instruction.type),
                lowerType(lower.global, instruction.type),
                instruction.name
            );
            break;
        }
        case Value::Copy: {
            // A copy is a real duplicate, so unlike a move it needs storage of its own: an
            // aggregate is a block copy into a fresh alloca and a scalar is an ordinary load,
            // which is already a fresh value in a register.
            auto& copied = (InstCopy&)instruction;
            assertTrue(copied.copy == nullptr);

            auto address = lowerPlace(lower, block, *function, copied.place);

            if(isMemoryType(lower.global, instruction.type)) {
                auto size = typeSize(lower.global, instruction.type);
                auto bytes = immediate(lower, size);
                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                    instruction.name, bytes, typeAlign(lower.global, instruction.type)));

                auto target = allocation->created().ptr - lower.lower;
                auto count = immediate(lower, size);
                auto blockCopy = block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(target, address, count));
                blockCopy->source = instruction.source;

                lower.values.add(instValue, target);
                return;
            }

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                memoryWidth(lower.global, instruction.type),
                signedType(lower.global, instruction.type),
                lowerType(lower.global, instruction.type),
                instruction.name
            );
            break;
        }
        case Value::Drop: {
            /*
             * Two halves, either of which may be absent: run what the value's lifetime ends in, and
             * hand back the storage it occupied. A drop with neither is one the pass should have
             * elided rather than emitted.
             *
             * The order is the one the language requires - whatever the drop does, it does while
             * the storage is still there to do it in.
             */
            auto& dropped = (InstDrop&)instruction;
            assertTrue(!dropped.isEmpty());
            assertTrue(dropped.flag == maxLimit<U32>);

            auto address = lowerPlace(lower, block, *function, dropped.place);

            auto callWith = [&](ModulePtr<Function> callee) {
                auto target = lower.functions.getValue(callee).unwrap();
                auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, target));

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

            step(dropped.drop);
            step(dropped.reclaim);

            // Handing back this allocation is the last thing that happens to it, after both halves
            // have finished reading it.
            if(dropped.releaseStorage) step(lower.from.freeHeap);

            break;
        }
        case Value::Address: {
            // Nothing is loaded: the address the place computes is the value.
            auto address = lowerPlace(lower, block, *function, ((InstAddress&)instruction).place);
            lower.values.add(instValue, address);
            return;
        }
        case Value::Native: {
            auto& native = (InstNative&)instruction;
            Array<LowerPtr<LowerValue>> args;
            for(auto arg: native.args.contents(lower.local)) args.push(mappedValue(lower, arg));

            switch(native.op) {
                case NativeOp::CopyMemory:
                    result = block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(args[0], args[1], args[2]));
                    break;
                case NativeOp::SetMemory:
                    // setMemory is written (to, value, count) and the instruction takes
                    // (to, count, pattern), which is the order its printed form uses.
                    result = block.addInst(lower.lower, new (lower.to.arena) LowerInstSetPattern(args[0], args[2], args[1]));
                    break;
                case NativeOp::Syscall: {
                    // The kernel is the callee, so there is no function operand: the number is
                    // operand zero, exactly as the lower IR's own syscall form has it.
                    auto created = isUnit(lower.global, instruction.type) ? 0 : 1;

                    result = call(lower.lower, lower.to, block, created, args.size(), LowerCallType::Syscall,
                                  [&](LowerInstCall* syscall) {
                        if(created) {
                            new (syscall->created().ptr) LowerValue(syscall, lowerType(lower.global, instruction.type),
                                                                    instruction.name);
                        }

                        for(Size i = 0; i < args.size(); i++) syscall->used()[i] = args[i];
                    });

                    break;
                }
            }

            break;
        }
        case Value::Cast: {
            auto& castInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, castInst.from);
            auto sourceType = lower.local[castInst.from]->type;

            // A conversion involving a raw pointer moves no bits: both sides are one machine
            // word, and what changes is only what the program says the word means.
            if(isPointer(lower.global, sourceType) || isPointer(lower.global, instruction.type)) {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstUnary(
                    LowerInst::Bitcast, instruction.name, lowerType(lower.global, instruction.type), from));
                break;
            }

            auto sourceLower = lowerType(lower.global, sourceType);
            auto targetLower = lowerType(lower.global, instruction.type);

            auto integerWiden = isInteger(lower.global, sourceType) &&
                                isInteger(lower.global, instruction.type) &&
                                sourceLower == LowerType::Int32 &&
                                targetLower == LowerType::Int64;

            auto signedSource = signedType(lower.global, sourceType) &&
                                (integerWiden || isFloat(lower.global, instruction.type));

            auto signedResult = signedType(lower.global, instruction.type) &&
                                (integerWiden || isFloat(lower.global, sourceType));

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstCast(instruction.name, targetLower, from, signedSource, signedResult));
            break;
        }
        case Value::Neg:
        case Value::Not: {
            auto& unaryInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, unaryInst.from);
            if(instruction.kind == Value::Neg) {
                result = unary<LowerInst::Neg>(
                    lower.lower, lower.to, block, lower.lower[from],
                    lowerType(lower.global, instruction.type),
                    instruction.name
                );
            } else {
                result = unary<LowerInst::Not>(
                    lower.lower, lower.to, block, lower.lower[from],
                    lowerType(lower.global, instruction.type),
                    instruction.name
                );
            }
            break;
        }
        case Value::Add:
        case Value::Sub:
        case Value::Mul:
        case Value::Div:
        case Value::Rem:
        case Value::Shl:
        case Value::Shr:
        case Value::Sar:
        case Value::And:
        case Value::Or:
        case Value::Xor: {
            auto& binaryInst = (InstBinary&)instruction;
            auto lhs = mappedValue(lower, binaryInst.lhs);
            auto rhs = mappedValue(lower, binaryInst.rhs);
            auto type = lowerType(lower.global, instruction.type);

            switch(binaryKind(lower, binaryInst)) {
                case LowerInst::Add:
                    result = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Sub:
                    result = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Mul:
                    result = binary<LowerInst::Mul>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::IMul:
                    result = binary<LowerInst::IMul>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Div:
                    result = binary<LowerInst::Div>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::IDiv:
                    result = binary<LowerInst::IDiv>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::IRem:
                    result = binary<LowerInst::IRem>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Rem:
                    result = binary<LowerInst::Rem>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Shl:
                    result = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Shr:
                    result = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Sar:
                    result = binary<LowerInst::Sar>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::And:
                    result = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Or:
                    result = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Xor:
                    result = binary<LowerInst::Xor>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                default:
                    break;
            }
            break;
        }
        case Value::Cmp: {
            auto& compare = (InstCmp&)instruction;
            auto lhs = mappedValue(lower, compare.lhs);
            auto rhs = mappedValue(lower, compare.rhs);

            result = cmp(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], lowerCmp(lower, compare), instruction.name);
            break;
        }
        case Value::Call: {
            auto& callInst = (InstCall&)instruction;
            auto target = lower.functions.getValue(callInst.callee).unwrap();
            auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, target));
            auto memoryResult = isMemoryType(lower.global, instruction.type);
            LowerPtr<LowerValue> returnPlace = nullptr;

            if(memoryResult) {
                auto bytes = immediate(lower, typeSize(lower.global, instruction.type));
                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, typeAlign(lower.global, instruction.type)));
                returnPlace = allocation->created().ptr - lower.lower;
            }

            auto created = isUnit(lower.global, instruction.type) || memoryResult ? 0 : 1;
            auto used = callInst.args.size() + 1 + (memoryResult ? 1 : 0);

            result = call(lower.lower, lower.to, block, created, used, lower.lower[target]->callType, [&](LowerInstCall* call) {
                if(created) {
                    new (call->created().ptr) LowerValue(call, lowerType(lower.global, instruction.type), instruction.name);
                }

                call->used()[0] = fun->created().ptr - lower.lower;

                Size index = 1;
                if(memoryResult) call->used()[index++] = returnPlace;

                for(auto arg: callInst.args.contents(lower.local)) {
                    call->used()[index++] = mappedValue(lower, arg);
                }
            });

            if(memoryResult) {
                result->source = instruction.source;
                lower.values.add(instValue, returnPlace);
                return;
            }

            break;
        }
        default:
            assertTrue("unexpected non-control resolve instruction" == nullptr);
            return;
    }

    mapResult(lower, instValue, result);
}

static void lowerTerminator(LowerContext& lower, LowerBlock& block, ModulePtr<Inst> pointer) {
    auto& instruction = *lower.local[pointer];
    LowerInst* result = nullptr;
    switch(instruction.kind) {
        case Value::Je: {
            auto& branch = (InstJe&)instruction;
            result = je(lower.lower, lower.to, block,
                        lower.lower[mappedValue(lower, branch.cond)],
                        lower.lower[lower.blocks.getValue(branch.thenBlock).unwrap()],
                        lower.lower[lower.blocks.getValue(branch.elseBlock).unwrap()]);
            break;
        }
        case Value::Jmp: {
            auto& jump = (InstJmp&)instruction;
            result = jmp(lower.lower, lower.to, block,
                         lower.lower[lower.blocks.getValue(jump.target).unwrap()]);
            break;
        }
        case Value::Ret: {
            auto& returnInst = (InstRet&)instruction;
            auto functionPointer = lower.local[instruction.block]->function;
            auto function = lower.local[functionPointer];
            auto memoryResult = isMemoryType(lower.global, function->returnType);

            if(memoryResult && returnInst.value) {
                auto target = lower.returnPlaces.getValue(functionPointer).unwrap();
                auto source = mappedValue(lower, returnInst.value);
                auto countValue = immediate(lower, typeSize(lower.global, function->returnType));
                auto copyInst = block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(target, source, countValue));
                copyInst->source = instruction.source;
            }

            auto count = returnInst.value && !memoryResult ? 1 : 0;
            auto storage = lower.to.arena.alloc(sizeof(LowerInstRet) + sizeof(LowerPtr<LowerValue>) * count);
            auto returnLower = new (storage) LowerInstRet;
            returnLower->usedCount = count;

            if(count) returnLower->used()[0] = mappedValue(lower, returnInst.value);
            result = block.addInst(lower.lower, returnLower);
            break;
        }
        default:
            assertTrue("expected resolve terminator" == nullptr);
            return;
    }

    result->source = instruction.source;
}

static void lowerPhi(LowerContext& lower, LowerBlock& block, ModulePtr<InstPhi> pointer) {
    auto& phi = *lower.local[pointer];
    auto count = phi.inputs.size();
    auto storage = lower.to.arena.alloc(
        sizeof(LowerInstPhi) +
        sizeof(LowerPtr<LowerValue>) * count +
        sizeof(LowerPtr<LowerBlock>) * count);

    auto result = new (storage) LowerInstPhi(phi.name, lowerType(lower.global, phi.type));
    result->source = phi.source;
    result->usedCount = count;

    Size index = 0;
    for(auto input: phi.inputs.contents(lower.local)) {
        result->used()[index] = mappedValue(lower, input.value);
        result->sources()[index] = lower.blocks.getValue(input.block).unwrap();
        index++;
    }

    block.addInst(lower.lower, result);
    lower.values.add((ModulePtr<Value>)pointer, result->created().ptr - lower.lower);
}

// Lowering covers the whole program: a call from the root module into Core has to reach a
// LowerFunction, and the two live in the same arena precisely so that it can.
Ptr<LowerModule> lowerProgram(Context& context, Program& program) {
    auto result = Ptr<LowerModule>(new LowerModule(8 * 1024 * 1024));
    LowerContext lower {
        context, program, *result, *program.types, *program.arena, *result->arena
    };

    // The compiler-built tables whose relocations still have to be translated. Kept until every
    // function exists, since a TypeDesc holds the address of teardown glue that has not been
    // created yet when the table itself is.
    struct RelocatedGlobal {
        ModulePtr<Global> source;
        LowerPtr<LowerGlobal> target;
    };

    Array<RelocatedGlobal> relocated;

    // Globals come first: a function's very first instruction may take the address of one, and
    // the lower module resolves that by name.
    for(auto module: program.modules) {
        for(auto globalPointer: module->globalOrder.contents(lower.local)) {
            auto source = lower.local[globalPointer];
            if(!module->root && !source->used) continue;

            auto target = new (result->arena) LowerGlobal(source->name);
            target->mut = source->mut;

            if(source->contents.length) {
                // A compiler-built table brings its own bytes. Its relocations are translated
                // here rather than resolved: what they point at may not have been emitted yet,
                // and the address they hold is not known until the module is placed.
                auto size = source->contents.length;
                target->initialContents = ByteBuffer((Byte*)result->arena.alloc(size), size);
                copy(source->contents.ptr, target->initialContents.ptr, size);
            } else {
                // A scalar starts as the bytes of its constant and an aggregate as zeroes, which
                // is the same statement in both cases: the global's Repr, filled from `initial`.
                auto size = typeSize(lower.global, source->type);
                target->initialContents = ByteBuffer((Byte*)result->arena.alloc(size), size);
                set(target->initialContents.ptr, size, 0);

                if(isDirectType(lower.global, source->type)) {
                    copy((const Byte*)&source->initial, target->initialContents.ptr,
                         size < sizeof(U64) ? Size(size) : sizeof(U64));
                }
            }

            *result->globals.add(source->name).value = target - lower.lower;
            relocated.push(RelocatedGlobal { globalPointer, target - lower.lower });
        }
    }

    Array<ModulePtr<Function>> emitted;
    for(auto module: program.modules) {
        for(auto functionPointer: module->functionOrder.contents(lower.local)) {
            if(lower.local[functionPointer]->signature) continue;

            // A generic function has no machine code of its own: this milestone specializes every
            // call, so what reaches the backend is its instantiations.
            if(lower.local[functionPointer]->gen) continue;
            if(!module->root && !lower.local[functionPointer]->used) continue;
            emitted.push(functionPointer);
        }
    }

    for(auto functionPointer: emitted) {
        auto function = lower.local[functionPointer];

        auto target = result->addFunction(function->name);
        target->source = function->source;

        if(!isUnit(lower.global, function->returnType) && !isMemoryType(lower.global, function->returnType)) {
            target->returnTypes.push(result->arena, lowerType(lower.global, function->returnType));
        }

        lower.functions.add(functionPointer, target - lower.lower);
    }

    // Now that every function and every global exists, the tables that hold their addresses can say
    // which ones. The addresses themselves are still unknown - that is the loader's half.
    for(auto& entry: relocated) {
        auto source = lower.local[entry.source];
        auto target = lower.lower[entry.target];

        for(auto relocation: source->relocations.contents(lower.local)) {
            LowerDataRelocation translated;
            translated.offset = relocation.offset;

            if(relocation.function) {
                auto found = lower.functions.getValue(relocation.function);

                // A table naming a function nothing else reached is what keeps that function alive,
                // so this should not happen; leaving the slot null is still better than pointing it
                // at the wrong thing.
                if(!found) continue;
                translated.function = found.unwrap();
            } else if(relocation.global) {
                auto found = result->globals.getValue(lower.local[relocation.global]->name);
                if(!found) continue;
                translated.global = found.unwrap();
            } else {
                continue;
            }

            target->relocations.push(result->arena, translated);
        }
    }

    for(auto functionPointer: emitted) {
        auto function = lower.local[functionPointer];
        auto target = lower.lower[lower.functions.getValue(functionPointer).unwrap()];

        // An aggregate result is returned through storage the caller passes in, so it becomes a
        // leading pointer argument that every `ret` in the function copies into.
        if(isMemoryType(lower.global, function->returnType)) {
            auto returnPlace = target->addArg(lower.lower, 0, LowerType::Pointer);
            lower.returnPlaces.add(functionPointer, &returnPlace->result - lower.lower);
        }

        for(auto argPointer: function->args.contents(lower.local)) {
            auto arg = lower.local[argPointer];

            // A `&` parameter arrives as the address of the caller's storage whatever it holds, so
            // its lower type is a pointer even where the borrowed type is a register-sized scalar.
            // For a memory type the two answers already coincide.
            auto argType = arg->isMutableBorrow() ? LowerType::Pointer : lowerType(lower.global, arg->type);
            auto targetArg = target->addArg(lower.lower, arg->name, argType);
            targetArg->source = arg->source;
            lower.values.add((ModulePtr<Value>)argPointer, &targetArg->result - lower.lower);
        }

        for(auto blockPointer: function->blocks.contents(lower.local)) {
            auto sourceBlock = lower.local[blockPointer];
            auto targetBlock = target->addBlock(lower.lower, sourceBlock->name);
            targetBlock->source = sourceBlock->source;
            lower.blocks.add(blockPointer, targetBlock - lower.lower);
        }

        lower.constantBlock = lower.lower[lower.blocks.getValue(function->blocks.get(lower.local, 0)).unwrap()];
        prepareScalars(lower, functionPointer, *function);

        for(auto blockPointer: function->blocks.contents(lower.local)) {
            auto sourceBlock = lower.local[blockPointer];
            auto targetBlock = lower.lower[lower.blocks.getValue(blockPointer).unwrap()];

            for(auto phi: sourceBlock->phis.contents(lower.local)) {
                lowerPhi(lower, *targetBlock, phi);
            }

            for(auto instruction: sourceBlock->instructions.contents(lower.local)) {
                lowerInstruction(lower, *targetBlock, instruction);
            }

            if(sourceBlock->terminator) {
                lowerTerminator(lower, *targetBlock, sourceBlock->terminator);
            }
        }
    }

    return result;
}
