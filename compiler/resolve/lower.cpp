#include "lower.h"
#include "analyze.h"
#include "generic.h"
#include "witness.h"
#include "../repr/table.h"
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

    /*
     * This backend's layout answers, owned by this backend.
     *
     * Constructed in lowerProgram from nativeReprTarget() and named there rather than read off the
     * program, which is the difference between a target being *chosen* and being inferred: a table
     * hanging off Program had to guess from the compile mode which one it was for, so a native
     * lowering in a JS build would have measured everything with the wrong ruler. The JS backend
     * builds its own the same way, and the two never meet.
     */
    ReprTable& repr;
    HashMap<U32, LowerPtr<LowerFunction>> functions;
    HashMap<U32, LowerPtr<LowerBlock>> blocks;
    HashMap<U32, LowerPtr<LowerValue>> values;
    HashMap<U32, LowerPtr<LowerValue>> returnPlaces;
    LowerBlock* constantBlock = nullptr;

    // The fields of each scalarized local of the function being lowered, by local index and then
    // by field index. Empty for every local that kept its storage - see prepareScalars().
    Array<Array<LowerPtr<LowerValue>>> scalars;

    /*
     * The erased half, set only while a *generic* function is being lowered.
     *
     * An unspecialized body does not know what its type variables are, so everything it would have
     * read off a type - a size, an alignment, how to relocate or release a value - it reads out of
     * the environment its caller passed instead. `genEnv` is that environment's address, and
     * `genContext` is the compile-time schema that says which slot holds what.
     *
     * Both are null for an ordinary function, and every use of them below is guarded by that: the
     * concrete path is unchanged, which is what keeps the two forms comparable.
     */
    LowerPtr<LowerValue> genEnv = nullptr;
    GenEnv* genContext = nullptr;
    Module* genModule = nullptr;
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

/*
 * Layout, asked of the target rather than read off the type.
 *
 * This file is the resolve-to-lower translation, which is the first point in the pipeline that is
 * allowed to know how wide anything is - see compiler/repr/repr.h for why that line is drawn here.
 * Everything upstream reasons in field indices and constructor names; from here down it is offsets
 * and bytes.
 */
static U32 typeSize(LowerContext& lower, TypePtr type) {
    return lower.repr.sizeOf(type);
}

static U32 typeAlign(LowerContext& lower, TypePtr type) {
    return lower.repr.alignOf(type);
}

/*
 * The two descriptions of a compiler-built table, checked against each other rather than believed.
 *
 * A witness table is described twice on purpose. Erased code reads it by slot number, through
 * repr/table.h; a function value's teardown reads the same table from the typed IR, as field
 * projections into the tuple typeDescPlaceType and closureHeaderPlaceType describe. Those are the
 * same slots, so they had better be the same bytes - and they are computed by two different rules,
 * the table layout and the ordinary struct layout, which happen to agree.
 *
 * Checked here because here is the only place both exist. Resolve states the slots and has no
 * offsets; the JS backend has neither. A target whose struct rule disagreed with its table rule
 * would otherwise emit a teardown reading the wrong word, which is the kind of thing that shows up
 * as a crash in unrelated code long afterwards.
 */
static void checkTableTypes(LowerContext& lower) {
    auto root = lower.from.root;
    if(!root) return;

    auto& repr = lower.repr;
    auto& target = repr.target;

    auto descriptor = typeDescPlaceType(*root);
    for(U16 i = 0; i < TypeDescFields::kCount; i++) {
        assertTrue(repr.fieldOf(descriptor, i)->offset ==
                   tableSlotOffset(target, TypeDescFields::kWordCount, i));
    }

    assertTrue(repr.sizeOf(descriptor) ==
               tableSize(target, TypeDescFields::kWordCount, TypeDescFields::kCount));

    // The header has the stricter of the two requirements: the code generator places these bytes at
    // exactly this distance in front of an entry point, and the teardown subtracts the tuple's size
    // to find them - see teardownFunValue.
    auto header = closureHeaderPlaceType(*root);
    for(U16 i = 0; i < ClosureHeaderFields::kCount; i++) {
        assertTrue(repr.fieldOf(header, i)->offset ==
                   tableSlotOffset(target, ClosureHeaderFields::kWordCount, i));
    }

    assertTrue(repr.sizeOf(header) ==
               tableSize(target, ClosureHeaderFields::kWordCount, ClosureHeaderFields::kCount));
}

static U32 memoryWidth(LowerContext& lower, TypePtr type) {
    auto size = typeSize(lower, type);
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

/*
 * Reading the environment.
 *
 * Slot N is at a fixed offset and holds a pointer, so this is one load. That is the whole of
 * Implementation-Generics.md part 1's "no runtime name lookup": no hashing, no search, no
 * comparison - the schema decided the number at compile time and the code loads it.
 */
static LowerPtr<LowerValue> genSlot(LowerContext& lower, LowerBlock& block, U16 slot) {
    auto offset = tableSlotOffset(lower.repr.target, GenEnvFields::kWordCount,
                                  GenEnvFields::slot(slot));
    auto address = addOffset(lower, block, lower.genEnv, offset);
    auto loaded = load(lower.lower, lower.to, block, lower.lower[address], 8, false, LowerType::Pointer, 0);
    return loaded->created().ptr - lower.lower;
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
        auto offset = tableSlotOffset(lower.repr.target, ClassWitnessFields::kWordCount, U16(step));
        auto address = addOffset(lower, block, witness, offset);
        auto loaded = load(lower.lower, lower.to, block, lower.lower[address], 8, false, LowerType::Pointer, 0);
        witness = loaded->created().ptr - lower.lower;
    }

    return witness;
}

// The descriptor of a type this body cannot see, or null for one it can. A concrete type inside a
// generic body needs no descriptor: its size is a constant here exactly as it is anywhere else.
static LowerPtr<LowerValue> genTypeDesc(LowerContext& lower, LowerBlock& block, TypePtr type) {
    if(!lower.genEnv || !type || !isGeneric(lower.global, type)) return nullptr;

    auto slot = genTypeSlot(*lower.genModule, *lower.genContext, type);

    // A generic type with no slot is one the schema never recorded, which would mean the body needs
    // something its own context does not promise. requireTypeSlot is what keeps that from happening;
    // reaching it here is a compiler bug rather than a program error.
    assertTrue(slot != maxLimit<U16>);
    return genSlot(lower, block, slot);
}

// One U32 field of a descriptor, widened to the 64-bit form every size and offset is computed in.
static LowerPtr<LowerValue> descField(LowerContext& lower, LowerBlock& block,
                                      LowerPtr<LowerValue> descriptor, U16 slot) {
    auto offset = tableSlotOffset(lower.repr.target, TypeDescFields::kWordCount, slot);
    auto address = addOffset(lower, block, descriptor, offset);
    auto loaded = load(lower.lower, lower.to, block, lower.lower[address], 4, false, LowerType::Int32, 0);
    auto widened = cast<false, false>(lower.lower, lower.to, block,
                                      lower.lower[loaded->created().ptr - lower.lower],
                                      LowerType::Int64, 0);

    return widened->created().ptr - lower.lower;
}

// How many bytes one value of this type occupies - a constant where the type is known, and a load
// out of its descriptor where it is not.
static LowerPtr<LowerValue> sizeOfType(LowerContext& lower, LowerBlock& block, TypePtr type) {
    if(auto descriptor = genTypeDesc(lower, block, type)) {
        return descField(lower, block, descriptor, TypeDescFields::kSize);
    }

    return immediate(lower, typeSize(lower, type));
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
static LowerPtr<LowerValue> genEnvironment(LowerContext& lower, LowerBlock& block, InstGenCall& call) {
    if(call.env) return tableAddress(lower, block, call.env);

    auto& target = lower.repr.target;
    auto slots = call.fill;
    auto bytes = immediate(lower, tableSize(target, GenEnvFields::kWordCount,
                                            GenEnvFields::countFor(slots.size())));
    auto storage = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
        0, bytes, target.pointerAlign));

    auto base = storage->created().ptr - lower.lower;
    U16 index = 0;

    for(auto slot: slots.contents(lower.local)) {
        auto value = slot.isForwarded()
            ? genWitness(lower, block, slot.forwarded, slot.forwardedSupers)
            : tableAddress(lower, block, slot.constant);

        auto address = addOffset(lower, block, base, tableSlotOffset(
            target, GenEnvFields::kWordCount, GenEnvFields::slot(index)));
        block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, value, 8));
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
static LowerPtr<LowerValue> genMethod(LowerContext& lower, LowerBlock& block, InstGenCall& call) {
    auto witness = genWitness(lower, block, call.classSlot, call.classPath);
    auto argCount = U16(lower.global[lower.global[call.typeClass]->gen]->types.size());
    auto offset = tableSlotOffset(lower.repr.target, ClassWitnessFields::kWordCount,
                                  ClassWitnessFields::method(argCount, call.index));

    auto address = addOffset(lower, block, witness, offset);
    auto method = load(lower.lower, lower.to, block, lower.lower[address], 8, false, LowerType::Pointer, 0);

    return method->created().ptr - lower.lower;
}

// What a teardown's place holds. A generic teardown only ever names a whole local - a partial move
// is rejected long before here - so a root that is anything else is a concrete one by construction.
static TypePtr dropPlaceType(LowerContext& lower, Function& function, const Place& place) {
    auto projections = place.projections;
    if(place.root != PlaceRoot::Local || projections.isNotEmpty()) return nullptr;
    if(place.local >= function.localCount()) return nullptr;

    return function.localAt(lower.local, place.local).type;
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
            offset += lower.repr.of(type).payloadOffset;
            type = record->constructors.get(lower.global, projection.index).content;
        } else if(projection.kind == ProjectionKind::Field && lower.global[type]->kind == Type::Fun) {
            offset += FunValueLayout::offsetOf(projection.index);
            type = funValueFieldType(*lower.from.core, projection.index);
        } else if(projection.kind == ProjectionKind::Field) {
            auto field = lower.repr.fieldOf(type, projection.index);
            assertTrue(field != nullptr);
            offset += field->offset;
            type = field->type;
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

/*
 * A tag that is not stored anywhere.
 *
 * Everything above turns a place into an address, which every projection but one can be. A folded
 * discriminant cannot: there is no word holding it, and its value is a *fact about the payload's
 * bits* rather than something written next to them. So it is intercepted at the load and the store
 * instead of in the place walk, which is also where it belongs - `.discriminant` is the only
 * projection whose meaning is a computation.
 */

// The type a place's last projection is taken of, by the same rules lowerPlace walks by. Null when
// the place has no projections, which is a whole local and has no owner to speak of.
static TypePtr placeOwnerType(LowerContext& lower, Function& function, const Place& place) {
    TypePtr type;

    if(place.root == PlaceRoot::Global) {
        type = lower.local[place.global]->type;
    } else if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        auto referenced = lower.local[place.pointer]->type;
        type = place.root == PlaceRoot::Borrow
            ? ((BorrowType*)lower.global[referenced])->to
            : pointeeType(lower.global, referenced);
    } else {
        if(place.local >= function.localCount()) return nullptr;
        type = function.localAt(lower.local, place.local).type;
    }

    auto projections = place.projections;
    auto count = projections.size();
    if(!count) return nullptr;

    Size index = 0;
    for(auto projection: projections.contents(lower.local)) {
        if(index++ == count - 1) break;

        if(projection.kind == ProjectionKind::Discriminant) {
            type = lower.from.scalar.int_;
        } else if(projection.kind == ProjectionKind::Downcast) {
            type = ((RecordType*)lower.global[type])->constructors.get(lower.global, projection.index).content;
        } else if(projection.kind == ProjectionKind::Field && lower.global[type]->kind == Type::Fun) {
            type = funValueFieldType(*lower.from.core, projection.index);
        } else if(projection.kind == ProjectionKind::Field) {
            auto field = lower.repr.fieldOf(type, projection.index);
            if(!field) return nullptr;
            type = field->type;
        } else if(projection.kind == ProjectionKind::Deref) {
            type = pointeeType(lower.global, type);
        } else {
            return nullptr;
        }
    }

    return type;
}

// The record a place's final Discriminant projection is taken of, when that record's tag is folded
// into a niche. Null in every other case, which is every place in a program that has no folded type
// in it - so the cost of asking is a look at the last projection.
static TypePtr foldedTagRecord(LowerContext& lower, Function& function, const Place& place) {
    auto projections = place.projections;
    auto count = projections.size();
    if(!count) return nullptr;

    if(projections.get(lower.local, count - 1).kind != ProjectionKind::Discriminant) return nullptr;

    auto record = placeOwnerType(lower, function, place);
    if(!record || lower.global[record]->kind != Type::Record) return nullptr;

    return lower.repr.of(record).isNicheFolded() ? record : nullptr;
}

/*
 * Reading a folded tag: which constructor these bits are.
 *
 * The payload constructor is "the niche word holds something the payload could legally have
 * produced", and every other constructor is one specific pattern outside that range. So the test is
 * a range check, and the answer is a select rather than a branch - which is what keeps a folded
 * `Maybe` cheaper than the tag word it replaced rather than merely smaller.
 *
 * Computed in 64 bits throughout because a niche pattern can be any bit pattern of an eight-byte
 * word, and narrowed to the tag's own type at the end.
 */
static LowerPtr<LowerValue> decodeNicheTag(LowerContext& lower, LowerBlock& block,
                                           LowerPtr<LowerValue> payload, TypePtr record,
                                           TypePtr tagType, StringId name) {
    auto& repr = lower.repr.of(record);
    auto& encoding = repr.encoding;
    auto& niche = encoding.niche;

    auto constructors = ((RecordType*)lower.global[record])->constructors.size();
    auto payloadIndex = U64(encoding.payloadConstructor);

    auto address = addOffset(lower, block, payload, niche.offset);
    auto loaded = load(lower.lower, lower.to, block, lower.lower[address], niche.bytes, false,
                       LowerType::Int64, 0);
    auto word = loaded->created().ptr - lower.lower;

    /*
     * `word - validStart <= validEnd - validStart`, unsigned - one subtract and one compare for a
     * range test whichever end the valid patterns sit at. The subtract disappears for the usual
     * niche, whose valid range starts at zero.
     */
    auto relative = word;
    if(niche.validStart) {
        auto base = immediate(lower, niche.validStart);
        relative = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[word],
                                          lower.lower[base], LowerType::Int64, 0)->created().ptr - lower.lower;
    }

    auto span = immediate(lower, niche.validEnd - niche.validStart);
    auto inRange = cmp(lower.lower, lower.to, block, lower.lower[relative], lower.lower[span],
                       LowerCmp::le, 0)->created().ptr - lower.lower;

    auto tagLower = lowerType(lower.global, tagType);
    auto pick = [&](LowerPtr<LowerValue> whenInRange, LowerPtr<LowerValue> otherwise) {
        auto select = new (lower.to.arena) LowerInstSelect(name, whenInRange, otherwise, inRange, tagLower);
        block.addInst(lower.lower, select);
        return select->created().ptr - lower.lower;
    };

    // Two constructors is the shape this exists for - `Nothing`/`Just` and every `Result`-like type -
    // and there the pattern carries no information beyond "not the payload one". No arithmetic, then:
    // the answer is one of two constants.
    if(constructors == 2) {
        auto payloadTag = immediate(lower, payloadIndex, tagLower);
        auto otherTag = immediate(lower, payloadIndex == 0 ? 1 : 0, tagLower);
        return pick(payloadTag, otherTag);
    }

    /*
     * More than two, so which pattern it is decides which constructor it is. The patterns were handed
     * out to the non-payload constructors in index order, so recovering the ordinal recovers the
     * index - except that the payload constructor is missing from that sequence, which the last step
     * puts back.
     */
    auto first = immediate(lower, encoding.firstPattern);
    LowerPtr<LowerValue> ordinal;

    if(encoding.ascending) {
        ordinal = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[word],
                                         lower.lower[first], LowerType::Int64, 0)->created().ptr - lower.lower;
    } else {
        ordinal = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[first],
                                         lower.lower[word], LowerType::Int64, 0)->created().ptr - lower.lower;
    }

    auto narrowed = cast<false, false>(lower.lower, lower.to, block, lower.lower[ordinal],
                                       tagLower, 0)->created().ptr - lower.lower;

    // `ordinal >= payloadConstructor` means this constructor was written after the payload one, so
    // its index is one higher than its position in the pattern sequence.
    auto boundary = immediate(lower, payloadIndex, tagLower);
    auto shifted = cmp(lower.lower, lower.to, block, lower.lower[narrowed], lower.lower[boundary],
                       LowerCmp::ge, 0)->created().ptr - lower.lower;

    auto one = immediate(lower, 1, tagLower);
    auto bumped = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[narrowed],
                                         lower.lower[one], tagLower, 0)->created().ptr - lower.lower;

    auto adjust = new (lower.to.arena) LowerInstSelect(0, bumped, narrowed, shifted, tagLower);
    block.addInst(lower.lower, adjust);

    auto payloadTag = immediate(lower, payloadIndex, tagLower);
    return pick(payloadTag, adjust->created().ptr - lower.lower);
}

/*
 * Writing a folded tag, which for the payload constructor is writing nothing at all.
 *
 * That is not an optimization but the definition: the payload constructor *is* the payload's own
 * bits, so the only thing that could make it identifiable is the payload being written, which the
 * constructor's own field initializations do. Every other constructor has no payload to write, so
 * its pattern is the whole value.
 */
static void encodeNicheTag(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> payload,
                           TypePtr record, U64 constructor) {
    auto& encoding = lower.repr.of(record).encoding;
    if(constructor == encoding.payloadConstructor) return;

    auto address = addOffset(lower, block, payload, encoding.niche.offset);
    auto pattern = immediate(lower, encoding.patternOf(U16(constructor)));
    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, pattern, encoding.niche.bytes));
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

    auto& value = *lower.local[pointer];
    if(isConstant(value)) return mapConstant(lower, pointer);

    /*
     * How wide a concrete type is, which is a constant that only this stage knows.
     *
     * Materialized here rather than where the instruction sits, on the same terms as a literal: it
     * has no effect, no position, and one value however many times it is asked for. Doing it lazily
     * is what keeps the scaling fold above from leaving an `imm 1` behind every time it removes the
     * only use of a stride. A *generic* type's metric is a real load out of a descriptor and is not
     * this case; it is mapped where the instruction is.
     */
    if(value.kind == Value::TypeMetric) {
        auto& metric = (InstTypeMetric&)value;
        auto& repr = lower.repr.of(metric.of);
        auto number = metric.metric == TypeMetricKind::Align ? repr.align
                    : metric.metric == TypeMetricKind::Stride ? repr.stride
                    : repr.size;

        auto result = immediate(lower, number, lowerType(lower.global, value.type));
        lower.values.add(pointer, result);
        return result;
    }

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
static LowerInst* relocateWith(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> target,
                               LowerPtr<LowerValue> source, TypePtr type, ModulePtr<Function> sink,
                               bool erased) {
    if(erased && !sink) {
        auto descriptor = genTypeDesc(lower, block, type);
        auto slot = addOffset(lower, block, descriptor, tableSlotOffset(
            lower.repr.target, TypeDescFields::kWordCount, TypeDescFields::kMoveInit));
        auto moveInit = load(lower.lower, lower.to, block, lower.lower[slot], 8, false, LowerType::Pointer, 0);

        return call(lower.lower, lower.to, block, 0, 3, kDefaultCallType, [&](LowerInstCall* relocation) {
            relocation->used()[0] = moveInit->created().ptr - lower.lower;
            relocation->used()[1] = target;
            relocation->used()[2] = source;
        });
    }

    if(!sink) {
        auto count = sizeOfType(lower, block, type);
        return block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(target, source, count));
    }

    // Reachable because module.cpp's reachability walk follows the sink field, which is what puts
    // the callee in front of lowering at all.
    auto callee = lower.functions.getValue(sink).unwrap();
    auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, callee));

    return call(lower.lower, lower.to, block, 0, 3, lower.lower[callee]->callType, [&](LowerInstCall* sinkCall) {
        sinkCall->used()[0] = fun->created().ptr - lower.lower;
        sinkCall->used()[1] = target;
        sinkCall->used()[2] = source;
    });
}

static LowerInst* relocate(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> target,
                           ModulePtr<Value> value, LowerPtr<LowerValue> source, TypePtr type) {
    auto& produced = *lower.local[value];
    auto moved = produced.kind == Value::Move;
    auto sink = moved ? ((InstMove&)produced).sink : nullptr;

    /*
     * The erased case: a move of a value whose type this body cannot see.
     *
     * Which of Design-Memory §4.1's two relocations applies is exactly what the body has no way to
     * decide - the resolver left `sink` null because there was no concrete type to find one for -
     * so the answer travels in the descriptor its caller passed, and this is the load and the call
     * that read it. Unconditional, like the erased teardown and for the same reason: a TrivialSink
     * type's moveInit is a real function that copies its bytes rather than a null slot, so there is
     * nothing to test before calling.
     *
     * Only a move. Initializing storage from a copy or from a call result is a write of bytes that
     * already belong to nobody, and running a `Sink` there would empty a source that has none.
     */
    auto erased = moved && lower.genEnv && isGeneric(lower.global, type);
    return relocateWith(lower, block, target, source, type, sink, erased);
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

            auto bytes = sizeOfType(lower, block, instruction.type);

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

            // An alloca states its alignment at compile time, and a generic body has no type to ask
            // for one. Over-aligning is always safe and costs a few bytes of frame, so the erased
            // path takes the widest alignment any target ABI asks for rather than loading the real
            // one and having no way to use it.
            auto alignment = isGeneric(lower.global, instruction.type)
                ? 16u : typeAlign(lower, instruction.type);

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, alignment));
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

            // A folded tag is not in memory to be loaded - see decodeNicheTag. The place still
            // lowers to the payload's address, since a folded record's payload begins where the
            // record does.
            if(auto record = foldedTagRecord(lower, *function, loadInst.place)) {
                auto payload = lowerPlace(lower, block, *function, loadInst.place);
                lower.values.add(instValue, decodeNicheTag(lower, block, payload, record,
                                                           instruction.type, instruction.name));
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
                memoryWidth(lower, instruction.type),
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

            /*
             * Writing a folded tag, which is a store of a pattern or nothing at all.
             *
             * The constructor is always a literal here: a record is constructed by naming one, and
             * `place.discriminant = <computed>` is not something any front end can write. An
             * assertion rather than a fallback, because a runtime encode would be dead code that
             * nothing could ever exercise or test.
             */
            if(auto record = foldedTagRecord(lower, *function, init.place)) {
                auto& written = *lower.local[init.value];
                assertTrue(written.kind == Value::ConstInt);

                auto payload = lowerPlace(lower, block, *function, init.place);
                encodeNicheTag(lower, block, payload, record, ((ConstInt&)written).value);
                return;
            }

            auto address = lowerPlace(lower, block, *function, init.place);
            auto value = mappedValue(lower, init.value);

            if(isMemoryType(lower.global, lower.local[init.value]->type)) {
                result = relocate(lower, block, address, init.value, value, lower.local[init.value]->type);
            } else {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, value, memoryWidth(lower, lower.local[init.value]->type)));
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
                return;
            }

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                memoryWidth(lower, instruction.type),
                signedType(lower.global, instruction.type),
                lowerType(lower.global, instruction.type),
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
            auto a = lowerPlace(lower, block, *function, swap.a);
            auto b = lowerPlace(lower, block, *function, swap.b);
            auto erased = lower.genEnv && isGeneric(lower.global, swap.content);

            if(!isMemoryType(lower.global, swap.content)) {
                auto width = memoryWidth(lower, swap.content);
                auto isSigned = signedType(lower.global, swap.content);
                auto kind = lowerType(lower.global, swap.content);

                auto oldA = load(lower.lower, lower.to, block, lower.lower[a], width, isSigned, kind, 0);
                auto oldB = load(lower.lower, lower.to, block, lower.lower[b], width, isSigned, kind, 0);

                block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                    a, oldB->created().ptr - lower.lower, width));

                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                    b, oldA->created().ptr - lower.lower, width));

                break;
            }

            auto bytes = sizeOfType(lower, block, swap.content);
            auto alignment = isGeneric(lower.global, swap.content) ? 16u : typeAlign(lower, swap.content);
            auto temporary = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(0, bytes, alignment));
            auto slot = temporary->created().ptr - lower.lower;

            relocateWith(lower, block, slot, a, swap.content, swap.sink, erased);
            relocateWith(lower, block, a, b, swap.content, swap.sink, erased);
            result = relocateWith(lower, block, b, slot, swap.content, swap.sink, erased);
            break;
        }
        case Value::Exchange: {
            /*
             * Two relocations and no temporary. What is coming in is already a value rather than a
             * place - the caller moved it in - so there is nothing to save from being written over,
             * and the storage the old contents leave for is the result's own.
             */
            auto& exchange = (InstExchange&)instruction;
            auto address = lowerPlace(lower, block, *function, exchange.place);
            auto incoming = mappedValue(lower, exchange.value);
            auto content = instruction.type;

            if(!isMemoryType(lower.global, content)) {
                auto width = memoryWidth(lower, content);
                auto old = load(lower.lower, lower.to, block, lower.lower[address], width,
                                signedType(lower.global, content), lowerType(lower.global, content),
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
            relocateWith(lower, block, target, address, content, exchange.sink, erased);
            relocate(lower, block, address, exchange.value, incoming, content);

            lower.values.add(instValue, target);
            return;
        }
        case Value::Copy: {
            // A copy is a real duplicate, so unlike a move it needs storage of its own: an
            // aggregate is a block copy into a fresh alloca and a scalar is an ordinary load,
            // which is already a fresh value in a register.
            auto& copied = (InstCopy&)instruction;
            assertTrue(copied.copy == nullptr);

            auto address = lowerPlace(lower, block, *function, copied.place);

            if(isMemoryType(lower.global, instruction.type)) {
                auto bytes = sizeOfType(lower, block, instruction.type);
                auto alignment = isGeneric(lower.global, instruction.type)
                    ? 16u : typeAlign(lower, instruction.type);

                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                    instruction.name, bytes, alignment));

                auto target = allocation->created().ptr - lower.lower;
                auto count = sizeOfType(lower, block, instruction.type);
                auto blockCopy = block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(target, address, count));
                blockCopy->source = instruction.source;

                lower.values.add(instValue, target);
                return;
            }

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                memoryWidth(lower, instruction.type),
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

            /*
             * The erased case: a teardown the body cannot name, because the type it belongs to is
             * one of this function's own type variables. What runs is whatever the caller's
             * descriptor holds, reached through the same indirect call any function value uses -
             * the lower IR takes a call's callee as an ordinary operand, so a loaded address is as
             * good a callee as a symbol.
             *
             * The slots are never null, which is what keeps this branch-free: a type with nothing
             * to run gets the shared empty teardown rather than a hole, so the erased path is one
             * unconditional call per half instead of a load, a test and a split block. The flags in
             * the descriptor still say which halves are empty, for a later pass that wants to skip
             * the call rather than make it.
             */
            auto placeType = dropPlaceType(lower, *function, dropped.place);

            if(lower.genEnv && isGeneric(lower.global, placeType)) {
                auto descriptor = genTypeDesc(lower, block, placeType);

                auto erasedStep = [&](U16 slot) {
                    auto offset = tableSlotOffset(lower.repr.target, TypeDescFields::kWordCount, slot);
                    auto slotAddress = addOffset(lower, block, descriptor, offset);
                    auto loaded = load(lower.lower, lower.to, block, lower.lower[slotAddress], 8,
                                       false, LowerType::Pointer, 0);

                    if(result) result->source = instruction.source;
                    result = call(lower.lower, lower.to, block, 0, 2, kDefaultCallType,
                                  [&](LowerInstCall* teardown) {
                        teardown->used()[0] = loaded->created().ptr - lower.lower;
                        teardown->used()[1] = address;
                    });
                };

                erasedStep(TypeDescFields::kDrop);
                erasedStep(TypeDescFields::kReclaim);
                break;
            }

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
        case Value::TypeMetric: {
            /*
             * The layout question, answered.
             *
             * A concrete type folds to an immediate, exactly as the resolver used to fold it - so
             * `sizeOf(x)` costs nothing it did not cost before, and the difference is only in who
             * knew the number. A type variable has no number here, and the answer is a load out of
             * the descriptor its caller passed: `sizeOf` on a generic value works for the first
             * time, through machinery that already existed for the sizes lowering needed anyway.
             */
            auto& metric = (InstTypeMetric&)instruction;
            auto descriptor = genTypeDesc(lower, block, metric.of);

            // A concrete type's metric is a constant, so it is materialized on demand by
            // mappedValue rather than here - see the note there. Emitting it eagerly would leave an
            // `imm` behind for every one the scaling fold above removed the only use of.
            if(!descriptor) return;

            auto offset = metric.metric == TypeMetricKind::Align ? TypeDescFields::kAlign
                        : metric.metric == TypeMetricKind::Stride ? TypeDescFields::kStride
                        : TypeDescFields::kSize;

            lower.values.add(instValue, descField(lower, block, descriptor, offset));
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

            /*
             * Scaling by one, removed where the one became known.
             *
             * `p + n` on a `%U8` multiplies by the element stride, and the resolver no longer knows
             * that stride is 1 - it emits the question and this stage answers it. Answering with an
             * immediate and leaving the multiply behind would make byte arithmetic cost an
             * instruction it never used to, which Design.md's pointer section is explicit about, so
             * the fold moves here with the knowledge rather than being lost with it.
             *
             * Asked of the resolve operand rather than of the lowered one, so that the immediate is
             * never materialized at all - checking afterwards would leave a dead `imm 1` in the
             * constant block for every byte-pointer offset in the program.
             *
             * Deliberately narrow: only a metric this stage folded, never a `1` the program wrote.
             * The backends have constant folders; what this owes is the cost the pointer-arithmetic
             * idiom was promised, and nothing beyond it.
             */
            auto metricIsOne = [&](ModulePtr<Value> operand) {
                auto value = lower.local[operand];
                if(value->kind != Value::TypeMetric) return false;

                auto& metric = *(InstTypeMetric*)value;
                if(isGeneric(lower.global, metric.of)) return false;

                auto& repr = lower.repr.of(metric.of);
                auto number = metric.metric == TypeMetricKind::Align ? repr.align
                            : metric.metric == TypeMetricKind::Stride ? repr.stride
                            : repr.size;
                return number == 1;
            };

            if(instruction.kind == Value::Mul || instruction.kind == Value::Div) {
                // Division has only a right identity; multiplication has both.
                if(metricIsOne(binaryInst.rhs)) {
                    lower.values.add(instValue, mappedValue(lower, binaryInst.lhs));
                    return;
                }

                if(instruction.kind == Value::Mul && metricIsOne(binaryInst.lhs)) {
                    lower.values.add(instValue, mappedValue(lower, binaryInst.rhs));
                    return;
                }
            }

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
        case Value::Symbol: {
            // An address the loader supplies. The lower IR already has both forms, because a call
            // names its callee this way and a global load names its storage this way; what is new
            // here is only that the address is wanted as an ordinary value.
            auto& symbol = (InstSymbol&)instruction;

            if(symbol.callee) {
                auto target = lower.functions.getValue(symbol.callee).unwrap();
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(instruction.name, target));
            } else {
                auto global_ = lower.local[symbol.global];
                auto target = lower.to.globals.getValue(global_->name).unwrap();
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstGlobal(instruction.name, target));
            }

            break;
        }
        case Value::CallDyn: {
            /*
             * A call through an address, laid out exactly as a direct one: the callee first, then
             * the hidden result storage a memory result needs, then the environment, then the
             * declared arguments.
             *
             * The environment sits after the result place rather than before it because a lifted
             * lambda is an ordinary function whose first *declared* parameter it is - the caller
             * builds nothing hidden, and the two sides agree because both read the same list.
             */
            auto& callInst = (InstCallDyn&)instruction;
            LowerPtr<LowerValue> address = nullptr;
            LowerPtr<LowerValue> env = nullptr;

            if(callInst.callable) {
                // The two words the call is reached through. A function value is a memory type, so
                // its lowered form is the address of the three, and the code and the environment
                // are the first two loads off it.
                auto base = mappedValue(lower, callInst.callable);
                auto codeAddress = addOffset(lower, block, base, FunValueLayout::offsetOf(FunValueLayout::kCode));
                auto envAddress = addOffset(lower, block, base, FunValueLayout::offsetOf(FunValueLayout::kEnv));

                address = load(lower.lower, lower.to, block, lower.lower[codeAddress], 8, false,
                               LowerType::Pointer, 0)->created().ptr - lower.lower;

                env = load(lower.lower, lower.to, block, lower.lower[envAddress], 8, false,
                           LowerType::Pointer, 0)->created().ptr - lower.lower;
            } else {
                address = mappedValue(lower, callInst.address);
            }

            auto memoryResult = isMemoryType(lower.global, instruction.type);
            LowerPtr<LowerValue> returnPlace = nullptr;

            if(memoryResult) {
                auto bytes = sizeOfType(lower, block, instruction.type);
                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                    instruction.name, bytes, typeAlign(lower, instruction.type)));

                returnPlace = allocation->created().ptr - lower.lower;
            }

            Array<LowerPtr<LowerValue>> arguments;
            for(auto arg: callInst.args.contents(lower.local)) arguments.push(mappedValue(lower, arg));

            auto created = isUnit(lower.global, instruction.type) || memoryResult ? 0 : 1;
            auto used = arguments.size() + 1 + (env ? 1 : 0) + (memoryResult ? 1 : 0);

            result = call(lower.lower, lower.to, block, created, used, kDefaultCallType, [&](LowerInstCall* dynamic) {
                if(created) {
                    new (dynamic->created().ptr) LowerValue(dynamic, lowerType(lower.global, instruction.type), instruction.name);
                }

                dynamic->used()[0] = address;

                Size index = 1;
                if(memoryResult) dynamic->used()[index++] = returnPlace;
                if(env) dynamic->used()[index++] = env;

                for(auto argument: arguments) dynamic->used()[index++] = argument;
            });

            if(memoryResult) {
                result->source = instruction.source;
                lower.values.add(instValue, returnPlace);
                return;
            }

            break;
        }
        case Value::Call: {
            auto& callInst = (InstCall&)instruction;
            auto target = lower.functions.getValue(callInst.callee).unwrap();
            auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, target));
            auto memoryResult = isMemoryType(lower.global, instruction.type);
            LowerPtr<LowerValue> returnPlace = nullptr;

            if(memoryResult) {
                // The hidden result storage. Its size is a load rather than a constant wherever the
                // result type belongs to the caller's own type variables, which is the case
                // Implementation-Generics.md part 8 calls "owned return: hidden uninitialized
                // result pointer" - the caller provides it because only the caller knows where.
                auto bytes = sizeOfType(lower, block, instruction.type);
                auto alignment = isGeneric(lower.global, instruction.type)
                    ? 16u : typeAlign(lower, instruction.type);

                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, alignment));
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
        case Value::GenCall: {
            /*
             * The erased call - Implementation-Generics.md part 9.
             *
             * Structurally an ordinary call with one more argument in front: the environment the
             * callee reads its slots out of. Everything else about the shape is the same, which is
             * the point of the leading position - a caller does not have to know anything about the
             * callee's schema to lay out the call, only to have built the right environment.
             *
             * Reaching here means the environment was static, since that is the only case
             * emitGenericCall takes the erased path for. A forwarded or mixed environment - one
             * generic body calling another - specializes instead, and is what part 9's cases 2 and
             * 3 are still owed.
             */
            auto& callInst = (InstGenCall&)instruction;
            auto callee = lower.local[callInst.callee];

            /*
             * Two shapes reach here, and they differ only in where the code address comes from.
             *
             * A call to a generic *function* names it, and passes the environment the callee reads
             * its own slots out of. A deferred *class* dispatch names nothing: the implementation is
             * chosen by whoever supplied this function's environment, so the address is loaded out
             * of the witness sitting in one of its slots, and the callee - being a concrete thunk -
             * needs no environment of its own.
             */
            auto dispatched = callInst.typeClass != nullptr;
            LowerPtr<LowerValue> address = nullptr;
            LowerPtr<LowerValue> envValue = nullptr;

            if(dispatched) {
                address = genMethod(lower, block, callInst);
            } else {
                auto target = lower.functions.getValue(callInst.callee).unwrap();
                auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, target));
                address = fun->created().ptr - lower.lower;
                envValue = genEnvironment(lower, block, callInst);
            }

            /*
             * The concrete-to-erased boundary.
             *
             * The callee was compiled against its own type variables, so a parameter whose declared
             * type is one of them arrives as an address whatever the caller substituted - part 8's
             * "unknown-size values use addresses". A caller holding an `Int` in a register therefore
             * has to give it storage first.
             *
             * Done here rather than in the resolver on purpose: the typed IR stays the source of
             * truth for what the call *means*, and only its representation is adapted.
             */
            auto materialize = [&](LowerPtr<LowerValue> value, TypePtr concrete) {
                auto bytes = immediate(lower, typeSize(lower, concrete));
                auto storage = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                    0, bytes, typeAlign(lower, concrete)));

                auto address = storage->created().ptr - lower.lower;
                block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                    address, value, memoryWidth(lower, concrete)));

                return address;
            };

            Array<LowerPtr<LowerValue>> arguments;
            Size argIndex = 0;

            for(auto arg: callInst.args.contents(lower.local)) {
                auto value = mappedValue(lower, arg);
                auto concrete = lower.local[arg]->type;

                auto parameter = argIndex < callee->args.size()
                    ? lower.local[callee->args.get(lower.local, argIndex)] : nullptr;

                /*
                 * A `&` parameter is an address in both worlds, so there is nothing to adapt: the
                 * caller already passes a borrow. Boxing it would hand the callee the address of
                 * the borrow rather than of what was borrowed, and every write through it would
                 * land in a temporary the caller never reads - which is exactly the bug this
                 * condition exists to avoid.
                 */
                auto byAddress = parameter && parameter->isMutableBorrow();

                if(parameter && !byAddress && isGeneric(lower.global, parameter->type) &&
                   !isMemoryType(lower.global, concrete)) {
                    value = materialize(value, concrete);
                }

                arguments.push(value);
                argIndex++;
            }

            /*
             * The result, decided by what the *callee* declared rather than by what this call
             * substituted. A function returning `a` returns through caller storage however small the
             * substitution turns out to be, because the body it was compiled from has no other way
             * to hand a value back - so the caller provides the storage and reads out of it.
             */
            auto erasedResult = isMemoryType(lower.global, callee->returnType);
            auto concreteResult = isMemoryType(lower.global, instruction.type);
            LowerPtr<LowerValue> returnPlace = nullptr;

            if(erasedResult) {
                auto bytes = sizeOfType(lower, block, instruction.type);
                auto alignment = isGeneric(lower.global, instruction.type)
                    ? 16u : typeAlign(lower, instruction.type);

                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, alignment));
                returnPlace = allocation->created().ptr - lower.lower;
            }

            auto created = isUnit(lower.global, instruction.type) || erasedResult ? 0 : 1;
            auto used = arguments.size() + 1 + (envValue ? 1 : 0) + (erasedResult ? 1 : 0);

            result = call(lower.lower, lower.to, block, created, used, kDefaultCallType, [&](LowerInstCall* call) {
                if(created) {
                    new (call->created().ptr) LowerValue(call, lowerType(lower.global, instruction.type), instruction.name);
                }

                call->used()[0] = address;

                Size index = 1;
                if(envValue) call->used()[index++] = envValue;
                if(erasedResult) call->used()[index++] = returnPlace;

                for(auto argument: arguments) call->used()[index++] = argument;
            });

            if(erasedResult) {
                result->source = instruction.source;

                // Storage on the way in, a value on the way out: a result the caller can hold in a
                // register is loaded back out of the storage the erased signature made it use.
                if(concreteResult) {
                    lower.values.add(instValue, returnPlace);
                } else if(!isUnit(lower.global, instruction.type)) {
                    auto loaded = load(lower.lower, lower.to, block, lower.lower[returnPlace],
                                       memoryWidth(lower, instruction.type),
                                       signedType(lower.global, instruction.type),
                                       lowerType(lower.global, instruction.type), instruction.name);

                    lower.values.add(instValue, loaded->created().ptr - lower.lower);
                }

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
                // The other place bytes are written into storage that did not hold them: the
                // caller's hidden result slot. A returned move relocates into it by whatever rule
                // its type relocates by, exactly as an initialization does.
                auto target = lower.returnPlaces.getValue(functionPointer).unwrap();
                auto source = mappedValue(lower, returnInst.value);
                auto copyInst = relocate(lower, block, target, returnInst.value, source, function->returnType);
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
    ReprTable repr(*program.types, nativeReprTarget());

    LowerContext lower {
        context, program, *result, *program.types, *program.arena, *result->arena, repr
    };

    checkTableTypes(lower);

    /*
     * The compiler-built tables, kept until every function exists.
     *
     * Two things are deferred rather than done as each global is reached, and for the same reason: a
     * TypeDesc names teardown glue that has not been generated when the table itself is built, and
     * nothing has an address at all until the module is placed. So a slot naming a function becomes
     * a relocation, and the relocation is translated in a second pass over these.
     *
     * `offsets` is where each slot of the table landed, which is what turns a slot number into a
     * relocation offset. It is kept per table rather than recomputed because the layout rule is the
     * materializer's and asking it twice would be two chances to disagree.
     */
    struct RelocatedGlobal {
        ModulePtr<Global> source;
        LowerPtr<LowerGlobal> target;
        Array<U32> offsets;
    };

    Array<RelocatedGlobal> relocated;

    // The bytes of one global, wherever they end up going.
    auto lowerGlobal = [&](ModulePtr<Global> globalPointer) {
        auto source = lower.local[globalPointer];
        auto target = new (result->arena) LowerGlobal(source->name);
        target->mut = source->mut;

        Array<U32> offsets;

        if(source->isTable) {
            /*
             * A compiler-built table, laid out here rather than where it was described.
             *
             * This is the whole of what the structured form bought: resolve said which slot holds
             * the size and which holds the drop, and *this* target decides that an address is eight
             * bytes little-endian and that five words are followed by three of them. The JS backend
             * reads the same slots and never produces bytes at all.
             */
            Array<TableSlot> slots;
            for(auto slot: source->table.contents(lower.local)) slots.push(slot);

            target->initialContents = materializeTable(result->arena, lower.repr,
                                                       toBuffer(slots), offsets);
        } else {
            // A scalar starts as the bytes of its constant and an aggregate as zeroes, which
            // is the same statement in both cases: the global's Repr, filled from `initial`.
            auto size = typeSize(lower, source->type);
            target->initialContents = ByteBuffer((Byte*)result->arena.alloc(size), size);
            set(target->initialContents.ptr, size, 0);

            if(isDirectType(lower.global, source->type)) {
                /*
                 * Through a writer at the target's byte order rather than by copying the host's
                 * bytes. `initial` is a U64 of *storage* - see floatBits - and which of its bytes
                 * come first is a fact about whoever reads the emitted global, which is the target.
                 *
                 * A type narrower than the word takes the bytes the target would have put the value
                 * in: the leading ones little-endian, the trailing ones big-endian.
                 */
                auto order = lower.repr.target.byteOrder;

                Byte word[sizeof(U64)];
                Net::BufferWriter bits(word, sizeof(word));

                if(order == LittleEndian) {
                    bits.writeLong<LittleEndian>(source->initial);
                } else {
                    bits.writeLong<BigEndian>(source->initial);
                }

                auto width = size < sizeof(U64) ? Size(size) : sizeof(U64);
                auto first = order == LittleEndian ? 0 : sizeof(U64) - width;
                copy(word + first, target->initialContents.ptr, width);
            }
        }

        relocated.push(RelocatedGlobal { globalPointer, target - lower.lower, ::move(offsets) });
        return target;
    };

    // Globals come first: a function's very first instruction may take the address of one, and
    // the lower module resolves that by name.
    for(auto module: program.modules) {
        for(auto globalPointer: module->globalOrder.contents(lower.local)) {
            auto source = lower.local[globalPointer];
            if(!module->root && !source->used) continue;

            // A closure header is emitted in front of the function it belongs to rather than into
            // the module's data, so it is not one of these - see LowerFunction::prefix.
            if(source->prefixOf) continue;

            auto target = lowerGlobal(globalPointer);
            *result->globals.add(source->name).value = target - lower.lower;
        }
    }

    Array<ModulePtr<Function>> emitted;
    for(auto module: program.modules) {
        for(auto functionPointer: module->functionOrder.contents(lower.local)) {
            if(lower.local[functionPointer]->signature) continue;

            // A generic function has machine code of its own only when something takes the erased
            // path to it. Where every call site specialized, its instantiations are what reaches the
            // backend and the generic body is a compile-time artifact.
            if(lower.local[functionPointer]->gen && !lower.local[functionPointer]->genericallyUsed) continue;
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

        // A lifted lambda's closure header travels with it, because where those bytes go is stated
        // relative to this function's entry point and nowhere else.
        if(function->closureHeader) {
            target->prefix = lowerGlobal(function->closureHeader) - lower.lower;
        }

        lower.functions.add(functionPointer, target - lower.lower);
    }

    // Now that every function and every global exists, the tables that hold their addresses can say
    // which ones. The addresses themselves are still unknown - that is the loader's half.
    for(auto& entry: relocated) {
        auto source = lower.local[entry.source];
        auto target = lower.lower[entry.target];

        Size index = 0;

        for(auto slot: source->table.contents(lower.local)) {
            auto at = index++;
            if(!isAddressCell(slot.kind)) continue;

            LowerDataRelocation translated;
            translated.offset = entry.offsets[at];

            if(slot.function) {
                auto found = lower.functions.getValue(slot.function);

                // A table naming a function nothing else reached is what keeps that function alive,
                // so this should not happen; leaving the slot null is still better than pointing it
                // at the wrong thing.
                if(!found) continue;
                translated.function = found.unwrap();
            } else if(slot.global) {
                auto found = result->globals.getValue(lower.local[slot.global]->name);
                if(!found) continue;
                translated.global = found.unwrap();
            } else {
                // A deliberately empty address slot - "nothing to do", which is zeroes and no
                // relocation. See TableCell.
                continue;
            }

            target->relocations.push(result->arena, translated);
        }
    }

    for(auto functionPointer: emitted) {
        auto function = lower.local[functionPointer];
        auto target = lower.lower[lower.functions.getValue(functionPointer).unwrap()];

        /*
         * The two hidden parameters, in the order a caller writes them.
         *
         * The environment comes first because it is the one every generic function has, whatever
         * its signature: Implementation-Generics.md part 8's "every unspecialized generic function
         * receives `GenEnv*` as a hidden first argument". The result storage follows, for the same
         * reason it does in a concrete function - a value of unknown size cannot come back in a
         * register, so the caller says where to put it.
         */
        lower.genEnv = nullptr;
        lower.genContext = functionGen(lower.global, *function);
        lower.genModule = function->module;

        if(lower.genContext) {
            auto envArg = target->addArg(lower.lower, lower.context.addUnqualifiedName("genEnv", 6),
                                         LowerType::Pointer);
            lower.genEnv = &envArg->result - lower.lower;
        }

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
