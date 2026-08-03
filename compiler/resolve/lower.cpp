#include "lower.h"
#include "analyze.h"
#include "generic.h"
#include "place.h"
#include "witness.h"
#include "../repr/table.h"
#include "../opt/opt.h"
#include "../lower/lower_builder.h"
#include "../lower/lower_promote.h"

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

// What indexing homogeneous storage advances by, which is not always the size - see Repr. A run of
// `n` slots is `n` strides, never `n` sizes: the trailing padding of one element is what the next
// one's alignment needs, so measuring in sizes would overlap them.
static U32 typeStride(LowerContext& lower, TypePtr type) {
    return lower.repr.strideOf(type);
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

/*
 * Whether one parameter of a lowered signature exists at all.
 *
 * A unit value has no representation anywhere below resolve - lowerType has no answer for one and
 * mapResult never maps one - so a parameter of unit type is neither passed nor received. That is
 * not a corner case reserved for `fn f(x: {})`: a generic function specialized at `{}` grows one
 * wherever its signature named a type variable, which is what a lens whose block produces nothing
 * instantiates its continuation's result with. The caller and the callee have to leave the position
 * out by the same rule, or every argument after it shifts by one.
 *
 * A `&` parameter is the exception, and it is not really one: what travels there is the address of
 * the caller's storage rather than a value, and an address exists whatever it points at.
 */
static bool lowerArgExists(GlobalBase global, TypePtr type, bool mutableBorrow) {
    return mutableBorrow || !isUnit(global, type);
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
static LowerPtr<LowerValue> storageSize(LowerContext& lower, LowerBlock& block, TypePtr type) {
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
static LowerPtr<LowerValue> strideSize(LowerContext& lower, LowerBlock& block, TypePtr type) {
    if(auto descriptor = genTypeDesc(lower, block, type)) {
        return descField(lower, block, descriptor, TypeDescFields::kStride);
    }

    return immediate(lower, typeStride(lower, type));
}

// `count * stride`, folded where both are immediates - which is every run whose length the literal
// wrote down. The multiply survives only for a run whose size the program computes.
static LowerPtr<LowerValue> scaleBy(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> stride,
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
                                          lower.lower[count], LowerType::Int64, 0);
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
static LowerPtr<LowerValue> lowerPlace(LowerContext& lower, LowerBlock& block, Function& function,
                                       const Place& place, Size limit = maxLimit<Size>) {
    LowerPtr<LowerValue> address;

    if(place.root == PlaceRoot::Global) {
        auto global_ = lower.local[place.global];
        auto target = lower.to.globals.getValue(global_->name).unwrap();
        auto load = block.addInst(lower.lower, new (lower.to.arena) LowerInstGlobal(global_->name, target));

        address = load->created().ptr - lower.lower;
    } else if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        // A borrow is an address once the checking is done, so the two roots lower alike; all
        // that differed between them was how much could be proved before reaching here.
        address = mappedValue(lower, place.pointer);
    } else {
        assertTrue(place.local < function.localCount());

        address = mappedValue(lower, function.localAt(lower.local, place.local).value);
    }

    U32 offset = 0;

    /*
     * The path, over the walk everything shares - see resolve/place.h. What is this walk's own is
     * the offset and the address; the type each step arrives at is not, and used to be carried here
     * as well.
     *
     * `limit` stops before the trailing Property projection, which is how the *owner's* address is
     * asked for: a constrained field is reached by calling its witness with that address rather
     * than by adding anything to it. See propertySlotOf.
     */
    walkPlace(*lower.from.core, function, place, [&](const PlaceStep& step) {
        switch(step.kind) {
            case ProjectionKind::Discriminant:
                break;

            case ProjectionKind::Downcast:
                // A boxed payload sits at that offset as a pointer, and the Deref after this is
                // what loads through it - which is `step.type` already being the `%T`.
                offset += lower.repr.of(step.owner).payloadOffset;
                break;

            case ProjectionKind::Field:
                if(lower.global[step.owner]->kind == Type::Fun) {
                    offset += FunValueLayout::offsetOf(step.index);
                    break;
                }

                {
                    auto field = lower.repr.fieldOf(step.owner, step.index);
                    assertTrue(field != nullptr);
                    offset += field->offset;
                }

                break;

            case ProjectionKind::Unit:
                // The word a packed field lives in, which is the address the path has already
                // reached: a packed field's `offset` is its word's, so the Field in front of this
                // one spent it. Nothing is added - see unitBits, which is what reads the width back
                // out at the load and the store.
                break;

            case ProjectionKind::Index: {
                /*
                 * One element of a `[T *n]` - Implementation-Containers.md §6.
                 *
                 * The only projection whose step is a *value* rather than a constant, which is why
                 * it is the only one that cannot be accumulated into `offset`: the elements are `n`
                 * values at a stride and which one this is may not be known until it runs. So the
                 * constant part of the path is spent first, exactly as the Deref below spends it,
                 * and the scaled index is added to the address that produces.
                 *
                 * A constant index folds all the way back to a constant offset in `compiler/opt`,
                 * which is what makes an unrolled walk over a small array cost the same as a
                 * record's fields.
                 */
                auto stride = lower.repr.of(step.type).stride;

                auto from = addOffset(lower, block, address, offset);
                auto index = mappedValue(lower, step.value);
                auto scale = immediate(lower, stride);

                auto scaled = binary<LowerInst::Mul>(lower.lower, lower.to, block, lower.lower[index],
                                                     lower.lower[scale], LowerType::Int64, 0)
                    ->created().ptr - lower.lower;

                auto stepped = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[from],
                                                      lower.lower[scaled], LowerType::Pointer, 0)
                    ->created().ptr - lower.lower;

                address = stepped;
                offset = 0;
                break;
            }

            case ProjectionKind::Deref: {
                // The pointer stored here becomes the address the rest of the path is relative to,
                // so everything accumulated so far has to be spent before it is loaded.
                auto from = addOffset(lower, block, address, offset);
                auto loaded = load(lower.lower, lower.to, block, lower.lower[from], 8, false,
                                   LowerType::Pointer, 0);

                address = loaded->created().ptr - lower.lower;
                offset = 0;
                break;
            }

            case ProjectionKind::Property:
                assertTrue("unsupported place projection reached lowering" == nullptr);
                break;
        }

        return true;
    }, limit);

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

// What a place is rooted in, and the type its last projection is taken of. Both are the shared
// walk's - see resolve/place.h - which is what makes "by the same rules lowerPlace walks by" a fact
// rather than a hope: lowerPlace is that walk too.
static TypePtr placeRootedType(LowerContext& lower, Function& function, const Place& place) {
    return placeRootType(*lower.from.core, function, place);
}

static TypePtr placeOwnedType(LowerContext& lower, Function& function, const Place& place) {
    return placeOwnerType(*lower.from.core, function, place);
}

// The record a place's final Discriminant projection is taken of, or null where the place does not
// end in one - which is every place in a program with no sum type in it, so the cost of asking is a
// look at the last projection.
static TypePtr taggedRecord(LowerContext& lower, Function& function, const Place& place) {
    auto projections = place.projections;
    auto count = projections.size();
    if(!count) return nullptr;

    if(projections.get(lower.local, count - 1).kind != ProjectionKind::Discriminant) return nullptr;

    auto record = placeOwnedType(lower, function, place);
    if(!record || lower.global[record]->kind != Type::Record) return nullptr;

    return record;
}

// The same, narrowed to a record whose tag is folded into a niche - the one shape that has no tag in
// memory at all.
static TypePtr foldedTagRecord(LowerContext& lower, Function& function, const Place& place) {
    auto record = taggedRecord(lower, function, place);
    return record && lower.repr.of(record).isNicheFolded() ? record : nullptr;
}

// And to a record whose tag is a bit range of the word its payload shares - see scalarizeSum. The
// place still lowers to the record's own address, since a bit-tagged payload begins where the record
// does; what it does not lower to is something a load of the tag's *type* would be the right width of.
static TypePtr bitTagRecord(LowerContext& lower, Function& function, const Place& place) {
    auto record = taggedRecord(lower, function, place);
    return record && lower.repr.of(record).isBitTagged() ? record : nullptr;
}

/*
 * How many bytes of memory a place that names a tag actually names, or zero for a place that names
 * something else.
 *
 * A tag is the one thing a place can name whose width is not a fact about its type. Every other load
 * takes its width from what it produces; a Discriminant projection produces an `Int` whatever record
 * it was taken of, and what is in memory is however much storage that record's Repr spends on its
 * discriminant - four bytes for a payload-carrying sum, and one for a `Bool`. So the width comes from
 * the owner, and a tag load of a type narrower than `Int` zero-extends into it.
 */
static U32 discriminantWidth(LowerContext& lower, Function& function, const Place& place) {
    auto record = taggedRecord(lower, function, place);
    if(!record) return 0;

    // A tag word only. The other two shapes are intercepted before anything asks for a width, and
    // answering with the containing word's here would let a missed interception store over a payload
    // rather than fail - see decodeBitTag and decodeNicheTag.
    auto& repr = lower.repr.of(record);
    return repr.discriminant == DiscriminantKind::Word ? repr.discriminantBytes : 0;
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
static U16 propertySlotOf(LowerContext& lower, const Place& place) {
    auto projections = place.projections;
    auto count = projections.size();
    if(!count) return maxLimit<U16>;

    auto last = projections.get(lower.local, count - 1);
    return last.kind == ProjectionKind::Property ? last.index : maxLimit<U16>;
}

// One operation of a property witness, loaded out of the witness the environment slot holds. Two
// loads and no search, exactly as a class method dispatch is - see genMethod.
static LowerPtr<LowerValue> propertyOp(LowerContext& lower, LowerBlock& block, U16 slot, U16 field) {
    auto witness = genSlot(lower, block, slot);
    auto offset = tableSlotOffset(lower.repr.target, PropertyWitnessFields::kWordCount, field);
    auto address = addOffset(lower, block, witness, offset);
    auto loaded = load(lower.lower, lower.to, block, lower.lower[address], 8, false, LowerType::Pointer, 0);

    return loaded->created().ptr - lower.lower;
}

// `op(owner, other)`, which is the shape both halves of a property witness have: two addresses in,
// nothing out. What differs between read and set is which address is the field's storage.
static void callPropertyOp(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> op,
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
static void zeroStorage(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> address, U32 bytes) {
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
static LowerPtr<LowerValue> erasedStorage(LowerContext& lower, LowerBlock& block, TypePtr type,
                                          StringId name) {
    auto bytes = sizeOfType(lower, block, type);
    auto alignment = isGeneric(lower.global, type) ? 16u : typeAlign(lower, type);
    auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(name, bytes, alignment));

    return allocation->created().ptr - lower.lower;
}

/*
 * Which bits of which word a place names, where it names bits rather than a word.
 *
 * The same interception a folded tag needs and for the same reason: `lowerPlace` turns a place into
 * an address, and a packed field's storage is a bit range that no address names. What the place walk
 * *does* produce is the address of the containing word, because a packed FieldRepr's `offset` is the
 * word's and every field of a scalar aggregate sits at offset zero of it - so the two halves below
 * take that address and finish the job.
 *
 * The bit offsets *compose*, which is what whole-record scalarization needs from this: in
 * `data Two {f: Flags, g: Flags}` the field `g` is two bits at bit two of a byte, and `g.a` is one bit
 * at bit two of the same byte. So the walk accumulates rather than reading the last projection, and
 * the answer is the innermost value's width at the outermost word's address.
 *
 * `exists()` is false for every place that is not one, which is every place in a program with nothing
 * packed in it.
 */
/*
 * How wide the storage unit a place names is, for the places `compiler/opt` already took apart.
 *
 * Zero for every other place, which is every place in a build with the shared expansion off and
 * every one this file still expands for itself - a reference-rooted access, or a word too wide for
 * the seam the expansion stops at. See ProjectionKind::Unit.
 *
 * The width has to come from the projection rather than from the loaded type, because they are
 * deliberately not the same number: the access is a `U32` in a register whatever the unit is, so a
 * two-field byte is one byte of traffic and a `memoryWidth` of the type would read three bytes past
 * it.
 */
static U32 unitBits(LowerContext& lower, const Place& place) {
    auto projections = place.projections;
    if(projections.isEmpty()) return 0;

    auto last = projections.get(lower.local, projections.size() - 1);
    return last.kind == ProjectionKind::Unit ? last.index : 0;
}

struct PackedAccess {
    U32 wordBytes = 0;
    U32 bitOffset = 0;
    U32 bitWidth = 0;
    TypePtr type = nullptr;

    bool exists() const { return bitWidth != 0; }
};

static TypePtr narrowRefRoot(LowerContext& lower, Function& function, const Place& place);

static PackedAccess packedAccess(LowerContext& lower, Function& function, const Place& place) {
    if(!placeRootedType(lower, function, place)) return {};

    /*
     * A place rooted in a reference that carries a shift is not this: the word's address is not the
     * root's value, and where the bits start is half the caller's. Those places belong to
     * `narrowRefAccess`, and keeping the two disjoint here rather than ordering the checks at each
     * call site is what stops a callee dereferencing a reference's shift as though it were an address.
     */
    if(narrowRefRoot(lower, function, place)) return {};

    PackedAccess access;
    auto declined = false;
    auto type = placeRootedType(lower, function, place);

    // The path, over the shared walk - see resolve/place.h. What is this one's own is the bit range;
    // the placement questions it asks are all about `step.owner`, which the walk hands it.
    walkPlace(*lower.from.core, function, place, [&](const PlaceStep& step) {
        auto decline = [&]() {
            declined = true;
            return false;
        };

        if(step.broken) return decline();
        type = step.type;

        switch(step.kind) {
            case ProjectionKind::Field: {
                // A function value's words are laid out by FunValueLayout rather than by Repr, and
                // are never packed - see lowerPlace, which offsets them the same way.
                if(lower.global[step.owner]->kind == Type::Fun) {
                    if(access.exists()) return decline();
                    break;
                }

                auto field = lower.repr.fieldOf(step.owner, step.index);
                if(!field) return decline();

                // A boxed field is a whole pointer, so it is neither inside a packed word nor the
                // start of one. `packCandidate` already declines it; declining here keeps that a
                // fact this walk states rather than one it assumes.
                if(step.crossedBox && access.exists()) return decline();

                if(access.exists()) {
                    /*
                     * Already inside a bit range, so this field's placement is relative to it. An
                     * *unpacked* field of a scalar aggregate is the whole of it - a single-field
                     * record keeps its address, see scalarizeTuple - so it contributes no offset and
                     * the width is the value's own.
                     */
                    access.bitOffset += field->bitOffset;
                    access.bitWidth = field->isPacked()
                        ? field->bitWidth
                        : valueWidth(lower.global, field->type).logical;

                    if(!access.bitWidth) return decline();
                } else if(field->isPacked()) {
                    access.wordBytes = field->wordBytes;
                    access.bitOffset = field->bitOffset;
                    access.bitWidth = field->bitWidth;
                }

                break;
            }
            case ProjectionKind::Downcast:
                // A payload inside a bit range can only be a single-constructor record's, whose
                // payload begins where the record does. Anything else has a tag of its own and is
                // not a scalar - see valueWidth.
                if(access.exists() && lower.repr.of(step.owner).payloadOffset) return decline();
                break;

            case ProjectionKind::Discriminant:
                // A payload-free sum *is* its discriminant, so this names the same bits under
                // another type and moves nothing. A *bit-tagged* sum's tag is at a placement of its
                // own, and one is never inside a bit range - `scalarBits` is zero for it, so nothing
                // co-packs it - but declining is what keeps that a fact rather than an assumption.
                if(lower.repr.of(step.owner).isBitTagged()) return decline();
                break;

            case ProjectionKind::Deref:
                // The pointer stored here becomes what the rest of the path is relative to, so a
                // packed word passed on the way is not this place's. Nothing narrow is a pointer, so
                // this can only be reached from outside a bit range.
                if(access.exists()) return decline();
                break;

            default:
                // A property is answered by a call taking an address rather than by a place, and is
                // intercepted before this is asked - see propertySlotOf. An `Index` steps by a
                // value, which no bit range can be reached through.
                return decline();
        }

        return true;
    });

    if(declined) return {};

    access.type = type;
    return access;
}

// The mask that selects `bits` low bits. Kept in one place because the 64-bit case is the one that
// would be undefined if it were written inline as `(1 << bits) - 1`.
static U64 lowMask(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

/*
 * Reading a packed field: load the word, move the field to the bottom, discard everything else.
 *
 * Two shapes rather than one. An unsigned field shifts down and masks; a signed one shifts *up*
 * until its sign bit is the word's and then shifts arithmetically back down, which sign-extends and
 * masks in the same two instructions rather than needing a third.
 *
 * The mask covers the bits *above* the range, so a field ending where its word does has none: the
 * load is unsigned at `wordBytes`, so everything above the word is already zero and the shift that
 * brought the field down took the rest with it. Same condition as `decode` in opt/opt_pack.cpp, which
 * handles every access this one does not.
 */
static LowerPtr<LowerValue> decodePackedBits(LowerContext& lower, LowerBlock& block,
                                             LowerPtr<LowerValue> word, const PackedAccess& field,
                                             bool isSigned) {
    auto loaded = load(lower.lower, lower.to, block, lower.lower[word], field.wordBytes, false,
                       LowerType::Int64, 0);
    auto bits = loaded->created().ptr - lower.lower;

    if(isSigned) {
        auto up = immediate(lower, 64 - field.bitOffset - field.bitWidth);
        auto down = immediate(lower, 64 - field.bitWidth);

        auto high = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[bits],
                                           lower.lower[up], LowerType::Int64, 0)->created().ptr - lower.lower;

        return binary<LowerInst::Sar>(lower.lower, lower.to, block, lower.lower[high],
                                      lower.lower[down], LowerType::Int64, 0)->created().ptr - lower.lower;
    }

    auto value = bits;
    if(field.bitOffset) {
        auto shift = immediate(lower, field.bitOffset);
        value = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[value],
                                       lower.lower[shift], LowerType::Int64, 0)->created().ptr - lower.lower;
    }

    if(field.bitOffset + field.bitWidth >= U32(field.wordBytes) * 8) return value;

    auto mask = immediate(lower, lowMask(field.bitWidth));
    return binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[value],
                                  lower.lower[mask], LowerType::Int64, 0)->created().ptr - lower.lower;
}

static LowerPtr<LowerValue> decodePackedField(LowerContext& lower, LowerBlock& block,
                                              LowerPtr<LowerValue> word, const PackedAccess& field,
                                              TypePtr type, StringId name) {
    auto bits = decodePackedBits(lower, block, word, field, signedType(lower.global, type));
    auto result = lowerType(lower.global, type);

    if(signedType(lower.global, type)) {
        return cast<true, true>(lower.lower, lower.to, block, lower.lower[bits], result, name)
            ->created().ptr - lower.lower;
    }

    return cast<false, false>(lower.lower, lower.to, block, lower.lower[bits], result, name)
        ->created().ptr - lower.lower;
}

/*
 * A primitive integer whose declared width is narrower than the register the lower IR holds it in,
 * so a value of it does not fill its own storage and arithmetic can leave a result the type cannot
 * represent. `U8`/`I8`/`U16`/`I16` in a 32-bit register, and `WideInt` in a 64-bit one.
 *
 * **A primitive only, never a `@bits` refinement.** The language rule is that arithmetic happens at
 * the type's native size and `@bits` describes *storage*, so `x + 1` on a `@bits(3) U32` computes at
 * 32 bits and is masked when it is stored; masking the value as well would change what a comparison
 * of it sees. That distinction is why the test is `registerBits(width)` and not the type's own
 * `naturalStorageBits`: a refinement's own width is exactly what must not be wrapped to here.
 *
 * The `canonical` test is belt and braces rather than a fix for an observed bug. A refinement's
 * arithmetic already never reaches here, because every `@bits` type dispatches to the instances of
 * the type it refines and the resulting instruction is typed at *that* type - `x + 1` above is typed
 * `U32`, which fills its register. Verified by disabling this predicate entirely: the lowering of a
 * `@bits(3) U32` add is byte-identical either way. It is stated in the predicate anyway so that the
 * rule is the code's rather than a consequence of how dispatch happens to type a refinement today.
 *
 * `WideInt` is the case that made this necessary at all: 53 bits *is* its native size, because it is
 * declared as a primitive rather than as `@bits(53) I64`, so its own `Integral` instance is selected
 * and 53 is the width its arithmetic is defined at on every target. Without this it wrapped at 64
 * here and at 53 on JS.
 *
 * The sub-word widths were the same bug found later and from the other side. Nothing narrowed a
 * `U8` result, so `addU8(200, 100)` was 300 here - a value of a type that cannot hold it, in a
 * register whose high bits nothing had cleared - and 44 on JS, which masks every narrow integer at
 * its own width. Widening one to an `Int` afterwards is a `cast` that trusts a register the
 * arithmetic never actually narrowed, so the dirt propagated silently.
 */
static bool narrowerThanRegister(GlobalBase global, TypePtr type) {
    if(!type || global[type]->kind != Type::Int) return false;

    auto integer = (IntType*)global[type];
    if(integer->canonical || integer->width == IntType::Bool) return false;

    return integer->bits < IntType::registerBits(integer->width);
}

/*
 * Wrapping an arithmetic result back into a type narrower than the register that holds it.
 *
 * Only the operations that can leave the range: `and`, `or`, `xor`, `sar`, division and remainder
 * all map an in-range pair to an in-range result, and masking those would cost an instruction per
 * operation to compute a value that is already correct.
 *
 * `shr` is here for a different reason than the arithmetic five, and needs `zeroExtendsShiftOperand`
 * below as well - see its comment. On its own the wrap would be pointless, since a logical shift of
 * an already-masked operand is in range for every distance but zero.
 */
static bool wrapsAtDeclaredWidth(GlobalBase global, TypePtr type, Value::Kind kind) {
    switch(kind) {
        case Value::Add: case Value::Sub: case Value::Mul: case Value::Shl: case Value::Neg:
        case Value::Shr:
            break;
        default:
            return false;
    }

    return narrowerThanRegister(global, type);
}

/*
 * Zero-extending the operand of a logical right shift.
 *
 * `shr` is the one operation that reads a narrow value's *storage* rather than its value. A signed
 * type narrower than its register is held sign-extended - that is exactly what `truncateToWidth`
 * leaves behind - so shifting it right logically brings the register's own sign bits down into the
 * answer instead of zeroes. `((0 :: WideInt) - 1) \`shr\` 1` was 2^63-1 here and 2^52-1 on JS, and
 * an `I8` holding -1 would have had the same 24 bits of dirt in front of it.
 *
 * Masking afterwards cannot recover it: by then the bits that should have been zero are part of the
 * result. So the operand is masked first, and the result is re-signed by `truncateToWidth` like any
 * other - which does something only at a shift distance of zero, where the masked value can still
 * have its own sign bit set.
 */
static bool zeroExtendsShiftOperand(GlobalBase global, TypePtr type, Value::Kind kind) {
    return kind == Value::Shr && signedType(global, type) && narrowerThanRegister(global, type);
}

// The register-relative distance that puts a narrow type's sign bit in the register's, which is what
// makes shifting up and arithmetically back down a truncate and a sign-extend in two instructions.
static U32 signShift(GlobalBase global, TypePtr type) {
    auto integer = (IntType*)global[type];
    return IntType::registerBits(integer->width) - integer->bits;
}

static LowerPtr<LowerValue> maskToWidth(LowerContext& lower, LowerBlock& block,
                                        LowerPtr<LowerValue> value, TypePtr type, LowerType lowered) {
    auto mask = immediate(lower, lowMask(U32(((IntType*)lower.global[type])->bits)), lowered);
    return binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[value],
                                  lower.lower[mask], lowered, 0)->created().ptr - lower.lower;
}

static LowerInst* truncateToWidth(LowerContext& lower, LowerBlock& block, LowerInst* result,
                                  TypePtr type, LowerType lowered, StringId name) {
    auto value = result->created().ptr - lower.lower;

    if(!signedType(lower.global, type)) {
        auto mask = immediate(lower, lowMask(U32(((IntType*)lower.global[type])->bits)), lowered);
        return binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[value],
                                      lower.lower[mask], lowered, name);
    }

    auto distance = immediate(lower, signShift(lower.global, type), lowered);
    auto up = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[value],
                                     lower.lower[distance], lowered, 0)->created().ptr - lower.lower;

    return binary<LowerInst::Sar>(lower.lower, lower.to, block, lower.lower[up],
                                  lower.lower[distance], lowered, name);
}

/*
 * Writing a packed field: read the word, replace the field's bits, write it back.
 *
 * The load is deliberately here rather than anywhere earlier. Design.md's write-back rule is that
 * the word is read *at commit time*, which is the whole reason two co-packed fields borrowed across
 * one call do not lose an update - the second commit reads what the first one wrote. Hoisting this
 * load out of the read-modify-write, or caching a word across a call, reintroduces the classic C
 * bitfield hazard that the rule exists to make impossible.
 *
 * The incoming value is masked rather than checked. That is the same choice `@bits` makes at every
 * other store, for the same reason: the mask is what makes the surrounding niche true, so it is not
 * an optimization that a range check could replace.
 */
static void encodePackedField(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> word,
                              const PackedAccess& field, LowerPtr<LowerValue> value) {
    auto loaded = load(lower.lower, lower.to, block, lower.lower[word], field.wordBytes, false,
                       LowerType::Int64, 0);
    auto bits = loaded->created().ptr - lower.lower;

    auto widened = cast<false, false>(lower.lower, lower.to, block, lower.lower[value],
                                      LowerType::Int64, 0)->created().ptr - lower.lower;

    auto fieldMask = immediate(lower, lowMask(field.bitWidth));
    auto trimmed = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[widened],
                                          lower.lower[fieldMask], LowerType::Int64, 0)->created().ptr - lower.lower;

    auto placed = trimmed;
    if(field.bitOffset) {
        auto shift = immediate(lower, field.bitOffset);
        placed = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[trimmed],
                                        lower.lower[shift], LowerType::Int64, 0)->created().ptr - lower.lower;
    }

    auto clearMask = immediate(lower, ~(lowMask(field.bitWidth) << field.bitOffset));
    auto cleared = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[bits],
                                          lower.lower[clearMask], LowerType::Int64, 0)->created().ptr - lower.lower;

    auto merged = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[cleared],
                                        lower.lower[placed], LowerType::Int64, 0)->created().ptr - lower.lower;

    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(word, merged, field.wordBytes));
}

/*
 * A borrow that is an address plus a shift - Design.md's tier 2.
 *
 * `&T` for a narrow `T` refers to a bit range rather than to a word, so it carries where in that
 * word the range starts. The shift is at most five bits, because a packed field never straddles the
 * natural storage unit of its own width, and it travels in the address's spare high bits - see
 * ReprTarget::addressBits. A non-narrow `T` has a provably zero shift and is left exactly the
 * address it always was.
 *
 * What the callee does with one is entirely decided by the *type*: the unit to load is
 * `naturalStorageBits(bits)` and the mask is `bits`, both constants there. Only the shift is
 * unknown, which is what lets one compiled body serve a packed field, an unpacked one, and a whole
 * local of the same type.
 *
 * A reference to a whole *aggregate* is the same thing once the aggregate is a scalar: `&Flags` for
 * `data Flags {a: Bool, b: Bool}` is two bits at a shift, and reading `f.a` through one is the
 * reference's shift plus the constant bit offset the field has inside those two bits. That constant is
 * the callee's own - it comes from its Repr for the pointee type, which the caller never had to agree
 * about beyond the type itself - so `bitOffset` below is added at the access rather than carried.
 */
struct NarrowRef {
    LowerPtr<LowerValue> address;
    LowerPtr<LowerValue> shift;
    U32 unitBytes = 0;
    U32 bits = 0;
    bool isSigned = false;
};

/*
 * Which bits a place names when the place is rooted in a reference of that kind.
 *
 * `referenced` is the pointee type, which decides the unit to load; `bitOffset` and `bitWidth` are
 * where inside it the place ends up, composed exactly as `packedAccess` composes them - `&two.g` is a
 * reference to a `Flags`, and `g.a` through it is one bit at offset zero of two.
 */
struct NarrowRefAccess {
    TypePtr referenced = nullptr;
    TypePtr type = nullptr;
    U32 bitOffset = 0;
    U32 bitWidth = 0;

    bool exists() const { return referenced != nullptr; }
};

// Between an address and the integer its bits are, which moves nothing: `Cast` is the int/float
// conversion and refuses a pointer on either side, and this is the one that does not - see
// validateCast and validateBitcast.
static LowerPtr<LowerValue> reinterpret(LowerContext& lower, LowerBlock& block,
                                        LowerPtr<LowerValue> value, LowerType type) {
    auto instruction = block.addInst(lower.lower, new (lower.to.arena) LowerInstUnary(
        LowerInst::Bitcast, 0, type, value));

    return instruction->created().ptr - lower.lower;
}

// The pointee type of a place that *is* a reference of this kind, or null for every other place -
// which is every place in a program with no narrow borrow in it.
static TypePtr narrowRefRoot(LowerContext& lower, Function& function, const Place& place) {
    TypePtr referenced = nullptr;

    if(place.root == PlaceRoot::Borrow) {
        /*
         * The pointee, where the root really is a borrow.
         *
         * Checked rather than assumed, and the check is not defensive tidying: a place whose root is
         * `PlaceRoot::Borrow` but whose pointer value is *not* typed `&T` reaches here, and casting
         * one to `BorrowType` reads a `to` field out of unrelated bytes - a `TypePtr` made of
         * whatever was there, and a segfault the moment anything asks for its Repr.
         *
         * That it can happen at all was found by a string format, whose sink is borrowed and written
         * through several times; which producer builds the mismatched root was not chased further,
         * because the answer here does not depend on it. A root that is not a borrow of a narrow
         * pointee is not a narrow reference, which is the only thing this function is asking, so
         * null is the correct answer rather than a fallback - and the alternative is reading a type
         * out of bytes that are not one.
         */
        auto pointee = lower.local[place.pointer]->type;
        if(!pointee || lower.global[pointee]->kind != Type::Borrow) return nullptr;

        referenced = ((BorrowType*)lower.global[pointee])->to;
    } else if(place.root == PlaceRoot::Local && place.local < function.localCount()) {
        // The slot behind a `&` parameter, which holds the reference the caller passed rather than
        // storage of its own. Every other local *is* its storage and is not one of these.
        auto slot = function.localAt(lower.local, place.local);
        if(!slot.borrowed) return nullptr;

        referenced = slot.type;
    } else {
        return nullptr;
    }

    return referenced && isNarrowRepr(lower.repr.of(referenced)) ? referenced : nullptr;
}

/*
 * Which bits of a reference a place names, or nothing where the place is not rooted in one.
 *
 * The projections a reference to a *scalar* may carry are the ones that stay inside its bits: fields
 * of a scalar aggregate, the discriminant of a payload-free sum, and the payload of a single
 * constructor. Anything else - a `Deref`, a property, a payload with a tag word of its own - leaves
 * them, and a reference whose pointee is narrow cannot have one of those under it.
 */
static NarrowRefAccess narrowRefAccess(LowerContext& lower, Function& function, const Place& place) {
    auto referenced = narrowRefRoot(lower, function, place);
    if(!referenced) return {};

    NarrowRefAccess access;
    access.referenced = referenced;
    access.type = referenced;
    access.bitWidth = lower.repr.of(referenced).scalarBits;

    auto declined = false;

    // The path, over the shared walk - see resolve/place.h. `step.owner` is what this one used to
    // carry in `access.type`, and `step.crossedBox` is the boxed-edge question it used to read back
    // out of the field and the constructor for itself.
    walkPlace(*lower.from.core, function, place, [&](const PlaceStep& step) {
        auto decline = [&]() {
            declined = true;
            return false;
        };

        if(step.broken) return decline();

        // A pointer is not a bit range, and what is on the other side of it is reached by a load
        // rather than by a shift - so a path crossing a box leaves this shape entirely.
        if(step.crossedBox) return decline();

        switch(step.kind) {
            case ProjectionKind::Field: {
                auto field = lower.repr.fieldOf(step.owner, step.index);
                if(!field) return decline();

                access.bitOffset += field->bitOffset;
                access.bitWidth = field->isPacked()
                    ? field->bitWidth
                    : valueWidth(lower.global, field->type).logical;

                if(!access.bitWidth) return decline();
                break;
            }
            case ProjectionKind::Downcast:
                if(lower.repr.of(step.owner).payloadOffset) return decline();
                break;

            case ProjectionKind::Discriminant:
                // As in packedAccess: a bit-tagged sum is never behind a narrow reference, and this
                // is where that stops being something to remember.
                if(lower.repr.of(step.owner).isBitTagged()) return decline();
                break;

            default:
                return decline();
        }

        access.type = step.type;
        return true;
    });

    if(declined) return {};

    return access.bitWidth ? access : NarrowRefAccess {};
}

// The word holding the reference, for a place narrowRefType answered for.
static LowerPtr<LowerValue> narrowRefValue(LowerContext& lower, Function& function, const Place& place) {
    if(place.root == PlaceRoot::Borrow) return mappedValue(lower, place.pointer);
    return mappedValue(lower, function.localAt(lower.local, place.local).value);
}

// The address and the shift, taken back apart. Two instructions, and the mask is the only constant
// the target contributes.
static NarrowRef unpackNarrowRef(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> ref,
                                 const NarrowRefAccess& access) {
    // The unit is the *pointee's*, not the accessed field's, which is what makes a field of a scalar
    // aggregate reachable through a reference to the whole of it: the aggregate's bits all sit inside
    // one such unit, so a load of it covers whichever field the constant below selects.
    auto unitBits = naturalStorageBits(lower.repr.of(access.referenced).scalarBits);
    auto addressBits = lower.repr.target.addressBits;

    // The bit arithmetic runs on an integer and the result becomes an address again. Only Add and
    // Sub take a pointer operand in the lower IR - see validateArith - which is the right rule and
    // is why this says what it is doing rather than relying on the two being the same width.
    auto word = reinterpret(lower, block, ref, LowerType::Int64);

    auto addressMask = immediate(lower, lowMask(addressBits));
    auto masked = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[word],
                                         lower.lower[addressMask], LowerType::Int64, 0)
        ->created().ptr - lower.lower;

    auto address = reinterpret(lower, block, masked, LowerType::Pointer);

    auto shiftBy = immediate(lower, addressBits);
    auto shift = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[word],
                                        lower.lower[shiftBy], LowerType::Int64, 0)
        ->created().ptr - lower.lower;

    // Where the field sits inside the pointee, added to where the pointee sits inside its unit. This
    // constant is the callee's own - it read it out of its Repr for a type it was told - so nothing
    // about it had to travel in the reference.
    if(access.bitOffset) {
        auto within = immediate(lower, access.bitOffset);
        shift = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[shift],
                                       lower.lower[within], LowerType::Int64, 0)
            ->created().ptr - lower.lower;
    }

    return NarrowRef {
        .address = address,
        .shift = shift,
        .unitBytes = unitBits / 8,
        .bits = access.bitWidth,
        .isSigned = signedType(lower.global, access.type),
    };
}

// Building one, at the borrow. `shift` is a constant here - the borrow site knows exactly which
// field it is taking - and folds away entirely for the unpacked case, where it is zero.
static LowerPtr<LowerValue> packNarrowRef(LowerContext& lower, LowerBlock& block,
                                          LowerPtr<LowerValue> address, U32 shift) {
    if(!shift) return address;

    auto word = reinterpret(lower, block, address, LowerType::Int64);

    auto tag = immediate(lower, U64(shift) << lower.repr.target.addressBits);
    auto tagged = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[word],
                                        lower.lower[tag], LowerType::Int64, 0)->created().ptr - lower.lower;

    return reinterpret(lower, block, tagged, LowerType::Pointer);
}

/*
 * A reference to something *inside* what a reference already names.
 *
 * `ref.shift` is where the value starts inside the word, which is the two halves of a reference added
 * together - the shift the caller passed plus the field's own offset. What is left is to re-split that
 * total against the unit the new pointee will be loaded in, since it may be narrower than the one the
 * old shift was measured against: four bits at bit 9 of a sixteen-bit unit are bit 1 of the second
 * byte, and a callee holding a `&@bits(4)` loads a byte.
 *
 * Every operand is a constant when the incoming shift was one, so a reborrow of a field of a whole
 * local costs nothing at all.
 */
static LowerPtr<LowerValue> stepNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                                          U32 unitBytes) {
    auto unitBits = unitBytes * 8;
    U32 unitLog = 0;
    while((U32(1) << unitLog) < unitBits) unitLog++;

    // (total / unitBits) * unitBytes, as two shifts, which is exact because both are powers of two.
    auto units = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[ref.shift],
                                        lower.lower[immediate(lower, unitLog)], LowerType::Int64, 0)
        ->created().ptr - lower.lower;
    auto step = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[units],
                                       lower.lower[immediate(lower, unitLog - 3)], LowerType::Int64, 0)
        ->created().ptr - lower.lower;

    auto address = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[ref.address],
                                          lower.lower[step], LowerType::Pointer, 0)
        ->created().ptr - lower.lower;

    auto within = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[ref.shift],
                                         lower.lower[immediate(lower, unitBits - 1)], LowerType::Int64, 0)
        ->created().ptr - lower.lower;

    auto word = reinterpret(lower, block, address, LowerType::Int64);
    auto tag = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[within],
                                      lower.lower[immediate(lower, lower.repr.target.addressBits)],
                                      LowerType::Int64, 0)->created().ptr - lower.lower;
    auto tagged = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[word],
                                        lower.lower[tag], LowerType::Int64, 0)->created().ptr - lower.lower;

    return reinterpret(lower, block, tagged, LowerType::Pointer);
}

// Reading through one: the same two shapes decodePackedField has, with the shift loaded rather than
// written in.
static LowerPtr<LowerValue> decodeNarrowBits(LowerContext& lower, LowerBlock& block, const NarrowRef& ref) {
    auto loaded = load(lower.lower, lower.to, block, lower.lower[ref.address], ref.unitBytes, false,
                       LowerType::Int64, 0);
    auto word = loaded->created().ptr - lower.lower;

    auto shifted = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[word],
                                          lower.lower[ref.shift], LowerType::Int64, 0)
        ->created().ptr - lower.lower;

    auto mask = immediate(lower, lowMask(ref.bits));
    auto masked = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[shifted],
                                         lower.lower[mask], LowerType::Int64, 0)->created().ptr - lower.lower;

    // Sign extension, where the type has a sign to extend: shift the value's top bit up to the
    // word's and bring it back arithmetically.
    if(ref.isSigned) {
        auto up = immediate(lower, 64 - ref.bits);
        auto high = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[masked],
                                           lower.lower[up], LowerType::Int64, 0)->created().ptr - lower.lower;

        return binary<LowerInst::Sar>(lower.lower, lower.to, block, lower.lower[high],
                                      lower.lower[up], LowerType::Int64, 0)->created().ptr - lower.lower;
    }

    return masked;
}

static LowerPtr<LowerValue> decodeNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                                            TypePtr type, StringId name) {
    auto bits = decodeNarrowBits(lower, block, ref);
    auto result = lowerType(lower.global, type);

    if(ref.isSigned) {
        return cast<true, true>(lower.lower, lower.to, block, lower.lower[bits], result, name)
            ->created().ptr - lower.lower;
    }

    return cast<false, false>(lower.lower, lower.to, block, lower.lower[bits], result, name)
        ->created().ptr - lower.lower;
}

/*
 * Writing through one, which is a read-modify-write of the unit and has no commit point.
 *
 * That is the whole of what makes this representation able to outlive the call that produced it:
 * there is no temporary to write back, so there is nothing whose lifetime has to be arranged. Every
 * write is complete when it returns, and two references into one unit interleave safely because each
 * reads the unit as it stands.
 */
static void encodeNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                            LowerPtr<LowerValue> value) {
    auto loaded = load(lower.lower, lower.to, block, lower.lower[ref.address], ref.unitBytes, false,
                       LowerType::Int64, 0);
    auto word = loaded->created().ptr - lower.lower;

    auto widened = cast<false, false>(lower.lower, lower.to, block, lower.lower[value],
                                      LowerType::Int64, 0)->created().ptr - lower.lower;

    auto mask = immediate(lower, lowMask(ref.bits));
    auto trimmed = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[widened],
                                          lower.lower[mask], LowerType::Int64, 0)->created().ptr - lower.lower;
    auto placed = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[trimmed],
                                         lower.lower[ref.shift], LowerType::Int64, 0)->created().ptr - lower.lower;

    auto hole = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[mask],
                                       lower.lower[ref.shift], LowerType::Int64, 0)->created().ptr - lower.lower;
    auto keep = unary<LowerInst::Not>(lower.lower, lower.to, block, lower.lower[hole],
                                      LowerType::Int64, 0)->created().ptr - lower.lower;

    auto cleared = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[word],
                                          lower.lower[keep], LowerType::Int64, 0)->created().ptr - lower.lower;
    auto merged = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[cleared],
                                        lower.lower[placed], LowerType::Int64, 0)->created().ptr - lower.lower;

    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(ref.address, merged, ref.unitBytes));
}

/*
 * Storage of its own, for a scalar aggregate that was living in someone else's word.
 *
 * Reading `two.g` produces a `Flags`, and a `Flags` is a memory type - every consumer of one expects
 * an address. What the bits were part of is a word this frame does not own the rest of, so the value
 * has to be somewhere before it can be handed over, and its own scalar storage is exactly as wide as
 * the bits are: `naturalBytes(scalarBits)`, with the fields at the same bit offsets they have here.
 *
 * That is the whole cost of scalarizing an aggregate rather than making it a register value. A
 * *direct* scalar record would need no storage at all, and this alloca is where that shows up - see
 * isDirectType, which is target-independent and therefore cannot know that this record became one.
 */
static LowerPtr<LowerValue> materializeScalar(LowerContext& lower, LowerBlock& block, TypePtr type,
                                              LowerPtr<LowerValue> bits, StringId name) {
    auto& repr = lower.repr.of(type);
    auto bytes = immediate(lower, repr.size);
    auto storage = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(name, bytes, repr.align));
    auto address = storage->created().ptr - lower.lower;

    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, bits, repr.size));
    return address;
}

// The other direction: the bits of a value about to be written into a word. A scalar aggregate arrives
// as the address of its own storage, so its bits are a load of it; everything else already is its
// bits. The mask that trims it to the field's width is applied by whoever merges, so a scalar with
// unused high bits needs nothing done to it here.
static LowerPtr<LowerValue> scalarBitsOf(LowerContext& lower, LowerBlock& block, TypePtr type,
                                         LowerPtr<LowerValue> value) {
    if(!type || !isMemoryType(lower.global, type)) return value;

    auto& repr = lower.repr.of(type);
    auto loaded = load(lower.lower, lower.to, block, lower.lower[value], repr.size, false,
                       LowerType::Int64, 0);

    return loaded->created().ptr - lower.lower;
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

/*
 * A tag that is a bit range of the word its payload sits in - see scalarizeSum.
 *
 * The third of the three tag shapes, and the one with the least of its own: where a folded tag is a
 * range check over the payload and a tag word is an ordinary load, this is exactly a co-packed field,
 * so both directions are the packed access path with the tag's own placement handed to it. What it
 * needs from the record's Repr is a `PackedAccess`, and that is all it needs.
 */
static PackedAccess bitTagAccess(const Repr& repr) {
    PackedAccess access;
    access.wordBytes = repr.discriminantBytes;
    access.bitOffset = repr.discriminantBitOffset;
    access.bitWidth = repr.discriminantBits;
    return access;
}

// Reading one. Unsigned whatever the tag's type is: a constructor index is a count, and a one-bit
// tag read as a signed field would decode constructor 1 as -1.
static LowerPtr<LowerValue> decodeBitTag(LowerContext& lower, LowerBlock& block,
                                         LowerPtr<LowerValue> word, TypePtr record,
                                         TypePtr tagType, StringId name) {
    auto access = bitTagAccess(lower.repr.of(record));
    auto bits = decodePackedBits(lower, block, word, access, false);

    return cast<false, false>(lower.lower, lower.to, block, lower.lower[bits],
                              lowerType(lower.global, tagType), name)->created().ptr - lower.lower;
}

// Writing one, which is a read-modify-write of the word and therefore preserves the payload sharing
// it - the same property that lets two co-packed fields be written independently.
static void encodeBitTag(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> word,
                         TypePtr record, U64 constructor) {
    auto access = bitTagAccess(lower.repr.of(record));
    encodePackedField(lower, block, word, access, immediate(lower, constructor));
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
                auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, target));

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
            if(isUnit(lower.global, instruction.type)) return;

            // Reading a field of a scalarized aggregate is naming the value that was written into
            // it, which is what makes the load disappear rather than become a cheaper load.
            if(isScalarPlace(lower, loadInst.place)) {
                lower.values.add(instValue, lower.scalars[loadInst.place.local][scalarField(lower, loadInst.place)]);
                return;
            }

            // A whole storage unit, which the shared expansion asked for by name: the shift and the
            // mask that used to follow are already instructions in front of this one, so all that is
            // left here is the load they read.
            if(auto bits = unitBits(lower, loadInst.place)) {
                auto address = lowerPlace(lower, block, *function, loadInst.place);
                lower.values.add(instValue, load(
                    lower.lower, lower.to, block, lower.lower[address], bits / 8, false,
                    lowerType(lower.global, instruction.type), instruction.name
                )->created().ptr - lower.lower);
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

            // A bit tag is in memory, but not at a width its type describes - see decodeBitTag.
            if(auto record = bitTagRecord(lower, *function, loadInst.place)) {
                auto word = lowerPlace(lower, block, *function, loadInst.place);
                lower.values.add(instValue, decodeBitTag(lower, block, word, record,
                                                         instruction.type, instruction.name));
                return;
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
                    return;
                }

                lower.values.add(instValue, load(
                    lower.lower, lower.to, block, lower.lower[out],
                    memoryWidth(lower, instruction.type),
                    signedType(lower.global, instruction.type),
                    lowerType(lower.global, instruction.type),
                    instruction.name
                )->created().ptr - lower.lower);
                return;
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
                    return;
                }

                lower.values.add(instValue, decodePackedField(lower, block, word, field,
                                                              instruction.type, instruction.name));
                return;
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
                    return;
                }

                lower.values.add(instValue, decodeNarrowRef(lower, block, ref, instruction.type,
                                                            instruction.name));
                return;
            }

            auto address = lowerPlace(lower, block, *function, loadInst.place);

            // An aggregate is never loaded into a value: the address of its storage is what the
            // rest of the lowering uses in its place.
            if(isMemoryType(lower.global, instruction.type)) {
                lower.values.add(instValue, address);
                return;
            }

            // A tag takes its width from the record it belongs to rather than from the `Int` the
            // projection produces - see discriminantWidth.
            auto tagWidth = discriminantWidth(lower, *function, loadInst.place);

            result = load(
                lower.lower, lower.to, block, lower.lower[address],
                tagWidth ? tagWidth : memoryWidth(lower, instruction.type),
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

            /*
             * Writing a value that carries nothing writes nothing.
             *
             * The resolver skips this where it can see it - a field of unit type is never written -
             * but it cannot see it through a type variable, and a specialization at `{}` is a clone
             * of instructions decided before the substitution. `Empty {only: value}.only` is the
             * shape: the constructor's content is `a`, and writing it is an Init the generic body
             * had every reason to emit.
             */
            if(isUnit(lower.global, lower.local[init.value]->type)) return;

            if(isScalarPlace(lower, init.place)) {
                lower.scalars[init.place.local][scalarField(lower, init.place)] = mappedValue(lower, init.value);
                return;
            }

            // The other half of the shared expansion: the merged word arrives already computed, so
            // this is the store it was computed for. The unit's width rather than the value's, for
            // the reason unitBits gives.
            if(auto bits = unitBits(lower, init.place)) {
                auto address = lowerPlace(lower, block, *function, init.place);
                block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                    address, mappedValue(lower, init.value), bits / 8));
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

            // A bit tag, written the way a co-packed field is: the word is read, the tag's bits are
            // replaced, and the payload sharing it comes back unchanged. Literal for the same reason
            // as above - nothing can write a computed constructor index.
            if(auto record = bitTagRecord(lower, *function, init.place)) {
                auto& written = *lower.local[init.value];
                assertTrue(written.kind == Value::ConstInt);

                auto word = lowerPlace(lower, block, *function, init.place);
                encodeBitTag(lower, block, word, record, ((ConstInt&)written).value);
                return;
            }

            /*
             * Writing a constrained field, which is the mirror image: the replacement goes into
             * storage of its own and the witness takes it from there.
             *
             * `set` consumes what it is handed, so this is a relocation into that storage rather
             * than a borrow of wherever the value already was - the callee commits it and releases
             * whatever the field held, and nothing here may release it a second time.
             */
            if(auto slot = propertySlotOf(lower, init.place); slot != maxLimit<U16>) {
                auto count = init.place.projections.size();
                auto owner = lowerPlace(lower, block, *function, init.place, count - 1);
                auto written = lower.local[init.value]->type;
                auto staging = erasedStorage(lower, block, written, 0);
                auto value = mappedValue(lower, init.value);

                if(isMemoryType(lower.global, written)) {
                    relocate(lower, block, staging, init.value, value, written);
                } else {
                    block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                        staging, value, memoryWidth(lower, written)));
                }

                callPropertyOp(lower, block, propertyOp(lower, block, slot, PropertyWitnessFields::kSet),
                               owner, staging);
                return;
            }

            // Written into a bit range rather than into storage. A scalar aggregate arrives as the
            // address of its own storage, so what goes into the word is a load of it - see
            // scalarBitsOf - and the merge masks it to the field's width either way.
            if(auto field = packedAccess(lower, *function, init.place); field.exists()) {
                auto word = lowerPlace(lower, block, *function, init.place);
                auto bits = scalarBitsOf(lower, block, lower.local[init.value]->type,
                                         mappedValue(lower, init.value));

                encodePackedField(lower, block, word, field, bits);
                return;
            }

            if(auto access = narrowRefAccess(lower, *function, init.place); access.exists()) {
                auto ref = unpackNarrowRef(lower, block, narrowRefValue(lower, *function, init.place),
                                           access);
                auto bits = scalarBitsOf(lower, block, lower.local[init.value]->type,
                                         mappedValue(lower, init.value));

                encodeNarrowRef(lower, block, ref, bits);
                return;
            }

            auto address = lowerPlace(lower, block, *function, init.place);
            auto value = mappedValue(lower, init.value);

            if(isMemoryType(lower.global, lower.local[init.value]->type)) {
                result = relocate(lower, block, address, init.value, value, lower.local[init.value]->type);
            } else {
                // As at the load: a tag is as wide as its record's Repr says, and the constructor
                // index being written is an `Int` whichever record it belongs to.
                auto tagWidth = discriminantWidth(lower, *function, init.place);
                auto width = tagWidth ? tagWidth : memoryWidth(lower, lower.local[init.value]->type);

                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(address, value, width));
            }

            break;
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
                return;
            }

            auto unitBytes = naturalStorageBits(lower.repr.of(referenced).scalarBits) / 8;

            // Reborrowing one: it is already a reference of this shape and carries its own shift, so
            // a reference to a *field* of what it names is the same word with the field's offset
            // added - and the total re-split against the unit the new pointee is loaded in.
            if(auto access = narrowRefAccess(lower, *function, borrow.place); access.exists()) {
                auto held = narrowRefValue(lower, *function, borrow.place);
                if(!access.bitOffset && unitBytes * 8 == naturalStorageBits(access.bitWidth)) {
                    lower.values.add(instValue, held);
                    return;
                }

                auto ref = unpackNarrowRef(lower, block, held, access);
                lower.values.add(instValue, stepNarrowRef(lower, block, ref, unitBytes));
                return;
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
                    return;
                }
            }

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
                    return;
                }

                result = ref.isSigned
                    ? cast<true, true>(lower.lower, lower.to, block, lower.lower[old],
                                       lowerType(lower.global, content), instruction.name)
                    : cast<false, false>(lower.lower, lower.to, block, lower.lower[old],
                                         lowerType(lower.global, content), instruction.name);
                break;
            }

            auto address = lowerPlace(lower, block, *function, exchange.place);

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

            /*
             * One traversal may serve both halves - Implementation-Containers.md §13.
             *
             * A container writes a single walk over its live elements, and which halves it supplies
             * is computed from the element types: `Array(Buffer)` has both, because the walk both
             * releases the run and runs each buffer's `Drop`. Running it twice would free the run
             * twice, so the second call is what is dropped rather than the second half.
             *
             * Which of them is elidable is unaffected. A region discharges the reclaim half in bulk
             * and leaves the drop half to run at last use, and this is the case where both are
             * present - so what runs there is one call, the drop's, and the release inside it does
             * nothing because the run's tag says the region owns the storage.
             */
            if(dropped.reclaim != dropped.drop) step(dropped.reclaim);

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
            SmallArray<LowerPtr<LowerValue>, 8> args;
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

                case NativeOp::HostCall:
                case NativeOp::HostField:
                case NativeOp::HostArray:
                    /*
                     * Unreachable by construction - Implementation-Containers.md §14.1.
                     *
                     * Every declaration that produces one of these is `@platform(js)`, and
                     * `platformEnabled` runs during resolution, so a native build has no name, no
                     * type and no instance that could reach one. Reaching here means the platform
                     * filter let a host declaration through, which is worth saying rather than
                     * approximating.
                     */
                    lower.context.diagnostics.error("internal: a host operation reached the native lowering"_v,
                                                    instruction.source);
                    break;
            }

            break;
        }
        case Value::Cast: {
            auto& castInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, castInst.from);
            auto sourceType = lower.local[castInst.from]->type;

            auto sourceLower = lowerType(lower.global, sourceType);
            auto targetLower = lowerType(lower.global, instruction.type);

            /*
             * A conversion between two addresses moves no bits: both sides are one machine word, and
             * what changes is only what the program says the word means.
             *
             * Asked of the *lowered* types rather than of `Type::Ptr` on either side, which is the
             * same question one level down and a strictly wider one. A raw pointer, a borrow and a
             * memory-typed value are all `LowerType::Pointer` here - they differ in what the checker
             * knows about them and not in what the machine holds - so a cast between any two of them
             * is a bitcast. The narrower test admitted only the first, which is all that existed
             * until `stringData` reinterpreted a `String` as a borrow of the record describing it
             * (Implementation-String.md part 2); that came out as a numeric conversion between two
             * pointers, which the lower IR validator rejects and rightly.
             */
            if(sourceLower == LowerType::Pointer || targetLower == LowerType::Pointer) {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstUnary(
                    LowerInst::Bitcast, instruction.name, targetLower, from));
                break;
            }

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

            if(zeroExtendsShiftOperand(lower.global, instruction.type, instruction.kind)) {
                lhs = maskToWidth(lower, block, lhs, instruction.type, type);
            }

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

            if(result && wrapsAtDeclaredWidth(lower.global, instruction.type, instruction.kind)) {
                result = truncateToWidth(lower, block, result, instruction.type, type, instruction.name);
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
                auto bytes = storageSize(lower, block, instruction.type);
                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                    instruction.name, bytes, typeAlign(lower, instruction.type)));

                returnPlace = allocation->created().ptr - lower.lower;
            }

            /*
             * The declared conventions come off the callee's *function type*, which is all a caller
             * reaching a function through a value has - and is what makes the two sides agree about
             * which positions exist without either consulting the other.
             *
             * `signature` and not `type`: the instruction's own type is what the call *produces*,
             * and reading a result type as a signature is only harmless while every result happens
             * to be a scalar whose bytes read back as an empty argument list. A continuation
             * returning an `Outcome` is not, which is how this was found.
             *
             * Null for the one caller that has no signature to give - a teardown reached through a
             * witness slot (analyze_teardown.cpp) - which falls back to each argument's own type,
             * the same answer the position-past-the-end case already produced.
             */
            auto signatureType = callInst.signature;
            auto signature = signatureType && lower.global[signatureType]->kind == Type::Fun
                           ? (FunType*)lower.global[signatureType] : nullptr;

            SmallArray<LowerPtr<LowerValue>, 8> arguments;
            Size dynIndex = 0;

            for(auto arg: callInst.args.contents(lower.local)) {
                auto declared = signature && dynIndex < signature->args.size()
                    ? signature->args.get(lower.global, dynIndex) : FunArg { lower.local[arg]->type };

                dynIndex++;
                if(!lowerArgExists(lower.global, declared.type,
                                   declared.convention == ast::BindType::Ref)) {
                    continue;
                }

                arguments.push(mappedValue(lower, arg));
            }

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
                auto bytes = storageSize(lower, block, instruction.type);
                auto alignment = isGeneric(lower.global, instruction.type)
                    ? 16u : typeAlign(lower, instruction.type);

                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, alignment));
                returnPlace = allocation->created().ptr - lower.lower;
            }

            // The positions this call actually passes, read off the callee's own parameters so
            // that it agrees with what the callee's signature above received.
            auto callee = lower.local[callInst.callee];
            SmallArray<LowerPtr<LowerValue>, 8> passed;
            Size callIndex = 0;

            for(auto arg: callInst.args.contents(lower.local)) {
                auto parameter = callIndex < callee->args.size()
                    ? lower.local[callee->args.get(lower.local, callIndex)] : nullptr;

                callIndex++;
                auto declared = parameter ? parameter->type : lower.local[arg]->type;

                if(!lowerArgExists(lower.global, declared, parameter && parameter->isMutableBorrow())) {
                    continue;
                }

                passed.push(mappedValue(lower, arg));
            }

            auto created = isUnit(lower.global, instruction.type) || memoryResult ? 0 : 1;
            auto used = passed.size() + 1 + (memoryResult ? 1 : 0);

            result = call(lower.lower, lower.to, block, created, used, lower.lower[target]->callType, [&](LowerInstCall* call) {
                if(created) {
                    new (call->created().ptr) LowerValue(call, lowerType(lower.global, instruction.type), instruction.name);
                }

                call->used()[0] = fun->created().ptr - lower.lower;

                Size index = 1;
                if(memoryResult) call->used()[index++] = returnPlace;

                for(auto argument: passed) call->used()[index++] = argument;
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

            SmallArray<LowerPtr<LowerValue>, 8> arguments;
            Size argIndex = 0;

            for(auto arg: callInst.args.contents(lower.local)) {
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
                auto declared = parameter ? parameter->type : concrete;

                argIndex++;

                /*
                 * Which positions exist is the *callee's* question here, and only here.
                 *
                 * The erased body was compiled against its own variables, so a parameter declared
                 * as one of them is a position in the signature whatever this caller substituted -
                 * including `{}`. Deciding by the concrete type instead would drop an argument the
                 * callee is still reading, so the two rules genuinely differ: a declared unit is
                 * absent, and a declared variable that happens to be unit here is present.
                 */
                if(!lowerArgExists(lower.global, declared, byAddress)) continue;

                if(isUnit(lower.global, concrete)) {
                    // Present, and carrying nothing. The callee takes the address and copies the
                    // size its type descriptor gives, which is zero - so what it points at never
                    // matters, only that it is an address at all. See storageSize.
                    auto storage = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                        0, storageSize(lower, block, concrete), 8));

                    arguments.push(storage->created().ptr - lower.lower);
                    continue;
                }

                auto value = mappedValue(lower, arg);

                if(parameter && !byAddress && isGeneric(lower.global, parameter->type) &&
                   !isMemoryType(lower.global, concrete)) {
                    value = materialize(value, concrete);
                }

                arguments.push(value);
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
                // The hidden result storage, which exists because the *callee's* signature said so:
                // a body returning `a` writes through caller storage however small `a` turns out to
                // be here, and `{}` is as small as it gets - see storageSize.
                auto bytes = storageSize(lower, block, instruction.type);
                auto alignment = isGeneric(lower.global, instruction.type) || isUnit(lower.global, instruction.type)
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

            /*
             * A unit result carries nothing back, whatever the resolve IR named.
             *
             * A body that returns unit *concretely* is resolved with no operand at all, but one
             * that returns a type variable is not: `fn identity(value: a) -> a` returns its
             * argument, and the specialization at `a = {}` is a `ret` naming a value nothing below
             * ever emitted - a unit value is not a value here. Read off the type rather than off
             * the operand, so that both spellings of "returns nothing" reach the same instruction.
             */
            auto returned = isUnit(lower.global, function->returnType) ? ModulePtr<Value>(nullptr)
                                                                       : returnInst.value;

            if(memoryResult && returned) {
                // The other place bytes are written into storage that did not hold them: the
                // caller's hidden result slot. A returned move relocates into it by whatever rule
                // its type relocates by, exactly as an initialization does.
                auto target = lower.returnPlaces.getValue(functionPointer).unwrap();
                auto source = mappedValue(lower, returned);
                auto copyInst = relocate(lower, block, target, returned, source, function->returnType);
                copyInst->source = instruction.source;
            }

            auto count = returned && !memoryResult ? 1 : 0;
            auto storage = lower.to.arena.alloc(sizeof(LowerInstRet) + sizeof(LowerPtr<LowerValue>) * count);
            auto returnLower = new (storage) LowerInstRet;
            returnLower->usedCount = count;

            if(count) returnLower->used()[0] = mappedValue(lower, returned);
            result = block.addInst(lower.lower, returnLower);
            break;
        }
        default:
            assertTrue("expected resolve terminator" == nullptr);
            return;
    }

    result->source = instruction.source;
}

/*
 * A phi, in the two halves its alternatives force it into.
 *
 * `createPhi` builds it detached and registers its result, because an alternative arriving over a
 * back edge is produced by a block this walk has not reached yet - and `mappedValue` is an assertion
 * rather than a lazy lookup for everything that is not a constant. Registering the result first is
 * also what lets the loop body use the phi it is producing an alternative for.
 *
 * `fillPhi` runs once the whole function is lowered, and `addInst` is deliberately part of it:
 * adding an instruction is what records its uses, so the alternatives have to exist by then.
 */
static LowerInstPhi* createPhi(LowerContext& lower, ModulePtr<InstPhi> pointer) {
    auto& phi = *lower.local[pointer];

    // Alternatives that agree on producing nothing join to nothing. A phi of unit type has no
    // lowered type to be built at and no input any block ever named, and every use of it is a use
    // that leaves the position out - see lowerArgExists.
    if(isUnit(lower.global, phi.type)) return nullptr;

    auto count = phi.inputs.size();
    auto storage = lower.to.arena.alloc(
        sizeof(LowerInstPhi) +
        sizeof(LowerPtr<LowerValue>) * count +
        sizeof(LowerPtr<LowerBlock>) * count);

    auto result = new (storage) LowerInstPhi(phi.name, lowerType(lower.global, phi.type));
    result->source = phi.source;
    result->usedCount = count;

    lower.values.add((ModulePtr<Value>)pointer, result->created().ptr - lower.lower);
    return result;
}

static void fillPhi(LowerContext& lower, LowerBlock& block, ModulePtr<InstPhi> pointer,
                    LowerInstPhi* result) {
    if(!result) return;

    auto& phi = *lower.local[pointer];

    Size index = 0;
    for(auto input: phi.inputs.contents(lower.local)) {
        result->used()[index] = mappedValue(lower, input.value);
        result->sources()[index] = lower.blocks.getValue(input.block).unwrap();
        index++;
    }

    block.addInst(lower.lower, result);
}

// Lowering covers the whole program: a call from the root module into Core has to reach a
// LowerFunction, and the two live in the same arena precisely so that it can.
Ptr<LowerModule> lowerProgram(Context& context, Program& program) {
    // The optimizer, against this target - see compiler/opt. Here rather than in the driver because
    // this function is what every native consumer reaches for, the fixture runner included, and an
    // optimization the tests do not see is one the tests do not check.
    optimizeProgram(context, program, nativeReprTarget());

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
        PackOffsets offsets;
    };

    Array<RelocatedGlobal> relocated;

    // The bytes of one global, wherever they end up going.
    auto lowerGlobal = [&](ModulePtr<Global> globalPointer) {
        auto source = lower.local[globalPointer];
        auto target = new (result->arena) LowerGlobal(source->name);
        target->mut = source->mut;

        PackOffsets offsets;

        if(source->isTable) {
            /*
             * A compiler-built table, laid out here rather than where it was described.
             *
             * This is the whole of what the structured form bought: resolve said which slot holds
             * the size and which holds the drop, and *this* target decides that an address is eight
             * bytes little-endian and that five words are followed by three of them. The JS backend
             * reads the same slots and never produces bytes at all.
             */
            SmallArray<TableSlot, 8> slots;
            for(auto slot: source->table.contents(lower.local)) slots.push(slot);

            target->initialContents = materializeTable(result->arena, lower.repr,
                                                       toBuffer(slots), offsets);
        } else if(source->literalBytes.length) {
            // A string literal's bytes, copied rather than described - see Global::literalBytes.
            // Resolve already encoded them into the target's native unit, so there is no layout
            // question left for this stage to answer.
            auto size = source->literalBytes.length;
            target->initialContents = ByteBuffer((Byte*)result->arena.alloc(size), size);
            copy(source->literalBytes.ptr, target->initialContents.ptr, size);
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

            // A parameter that carries nothing is not received - see lowerArgExists. Nothing maps
            // it either, so a body that names it has to be reaching it through a form that already
            // knows a unit value is not a value: a `ret` reads it off the return type, and every
            // call leaves the position out.
            if(!lowerArgExists(lower.global, arg->type, arg->isMutableBorrow())) continue;

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

        // Every phi in the function before any of them is filled in, so that an alternative coming
        // back around a loop is a value this walk has already heard of by the time it is asked for.
        SmallArray<LowerInstPhi*, 16> phis;
        for(auto blockPointer: function->blocks.contents(lower.local)) {
            for(auto phi: lower.local[blockPointer]->phis.contents(lower.local)) {
                phis.push(createPhi(lower, phi));
            }
        }

        for(auto blockPointer: function->blocks.contents(lower.local)) {
            auto sourceBlock = lower.local[blockPointer];
            auto targetBlock = lower.lower[lower.blocks.getValue(blockPointer).unwrap()];

            for(auto instruction: sourceBlock->instructions.contents(lower.local)) {
                lowerInstruction(lower, *targetBlock, instruction);
            }

            if(sourceBlock->terminator) {
                lowerTerminator(lower, *targetBlock, sourceBlock->terminator);
            }
        }

        Size phiIndex = 0;
        for(auto blockPointer: function->blocks.contents(lower.local)) {
            auto targetBlock = lower.lower[lower.blocks.getValue(blockPointer).unwrap()];

            for(auto phi: lower.local[blockPointer]->phis.contents(lower.local)) {
                fillPhi(lower, *targetBlock, phi, phis[phiIndex++]);
            }
        }

        // Every local got storage on the way here, because a place is an address and that is the only
        // shape this translation has. Which of those slots actually needed memory is a question about
        // the finished IR rather than about the source, so it is asked now - see lower_promote.h, and
        // isDirectType in resolve/type.h for why it is not asked any earlier.
        promoteStackSlots(lower.lower, *target);
    }

    return result;
}
