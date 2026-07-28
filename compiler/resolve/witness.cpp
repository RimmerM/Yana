#include "witness.h"
#include "analyze.h"
#include "expr.h"
#include "generic.h"
#include "name.h"

/*
 * Building the constants.
 *
 * Everything here follows the same three steps: allocate the bytes in the module arena, write the
 * scalar fields into them, and record a relocation for each address the table holds. The addresses
 * themselves are deliberately not resolved - a witness may name a function that has not been
 * generated yet, and none of them has an address at all until the module is placed.
 */

namespace {

// A global nothing in the source can name. Interned tables need a unique symbol for printing and
// linking, and this is the same trick addAnonymousFunction plays for the same reason.
Global* addAnonymousGlobal(Module& module, StringId name, LocationId source) {
    auto global_ = new (module.arena) Global(&module, name);
    global_->source = source;
    global_->type = module.scalar.unit;
    global_->used = true;
    module.globalOrder.push(module.arena, global_ - *module.arena);
    return global_;
}

struct TableBuilder {
    TableBuilder(Module& module, Size size): module(module) {
        bytes = ByteBuffer((Byte*)module.arena.alloc(size), size);
        set(bytes.ptr, size, 0);
    }

    void putU32(U32 offset, U32 value) {
        copy((const Byte*)&value, bytes.ptr + offset, sizeof(U32));
    }

    // An address is left as zeroes and recorded instead. A null target records nothing, which is
    // how "this type has no drop" reaches the reader as a null slot rather than as a missing entry.
    void putFunction(Global& target, U32 offset, ModulePtr<Function> function) {
        if(!function) return;

        (*module.arena)[function]->used = true;
        target.relocations.push(module.arena, GlobalRelocation { offset, function, nullptr });
    }

    // An interned type, stored as its region offset. Recorded as well as written, so that printing
    // can name the type rather than the number - see Global::typeWords.
    void putType(Global& target, U32 offset, TypePtr type) {
        putU32(offset, U32(type));
        target.typeWords.push(module.arena, offset);
    }

    void putGlobal(Global& target, U32 offset, ModulePtr<Global> global_) {
        if(!global_) return;

        (*module.arena)[global_]->used = true;
        target.relocations.push(module.arena, GlobalRelocation { offset, nullptr, global_ });
    }

    Module& module;
    ByteBuffer bytes;
};

/*
 * The teardown a type with nothing to run gets.
 *
 * A descriptor's lifecycle slots are never null, which is what lets erased code call them without
 * first testing them: one unconditional indirect call per half, instead of a load, a comparison and
 * a split block at every drop of a generic value. The flags still record which halves are empty, for
 * a pass that would rather skip the call than make it.
 *
 * One per program rather than one per type: it does nothing, so there is nothing to distinguish.
 */
ModulePtr<Function> emptyTeardown(Module& module, LocationId source) {
    auto& program = module.program;
    if(program.emptyTeardown) return program.emptyTeardown;

    auto& core = *program.core;
    auto function = addAnonymousFunction(core, module.context.addQualifiedName("teardown$none", 13, 1), source);
    function->returnType = core.scalar.unit;
    function->used = true;

    auto name = module.context.addQualifiedName("value", 5, 1);
    function->addArg(core, name, resolvePointerType(core, core.scalar.unit), source);

    ExprResolver resolver(core.context, core, *function);
    resolver.terminate(resolver.emit<InstRet>(source, 0, core.scalar.unit, nullptr));

    program.emptyTeardown = function - *core.arena;
    return program.emptyTeardown;
}

StringId tableName(Module& module, StringView prefix, TypePtr type) {
    StringBuilder text;
    text << prefix;
    describeType(module.context, *module.types, type, text);
    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

} // namespace

/*
 * moveInit.
 *
 * Implementation-Generics.md part 4 lists three answers - a block copy for TrivialSink, an adapter
 * over the authored `Sink` where one exists, and unavailable - and the first two are generated the
 * same way: a two-argument function taking the destination and the source as raw pointers, so that
 * generic code can call it without knowing either type or size.
 *
 * The "unavailable" case is not represented here at all, deliberately. Whether a body may move a
 * value of an unknown type is a question its *schema* answers, and it is answered before any of
 * this is reached; a descriptor that carried "you may not" would be inviting a second, later check
 * of something already settled.
 */
ModulePtr<Function> moveInitFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.moveInitGlue.get(U32(type))) return found.unwrap();

    auto ownership = ownershipOf(module, type);
    auto size = typeSize(*module.types, type);

    // Nothing to relocate: a zero-sized type, or one whose bytes the caller has already placed.
    if(!size) return nullptr;

    auto function = addAnonymousFunction(module, tableName(module, "moveInit$"_v, type), source);
    auto pointer = function - *module.arena;
    *program.moveInitGlue.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto pointerType = resolvePointerType(module, type);
    auto to = function->addArg(module, module.context.addQualifiedName("to", 2, 1), pointerType, source);
    auto from = function->addArg(module, module.context.addQualifiedName("from", 4, 1), pointerType, source);

    ExprResolver resolver(module.context, module, *function);

    if(ownership.trivialSink) {
        // A relocation of a TrivialSink value is its bytes and nothing else, which is what the
        // class means. copyMemory rather than a load and a store because the size is a constant
        // here and the type may be an aggregate with no register form at all.
        auto bytes = resolver.makeInt(source, module.scalar.long_, size);
        auto byteType = resolvePointerType(module, module.scalar.unit);

        auto castTo = resolver.ref(resolver.emit<InstUnary>(source, 0, byteType, Value::Cast,
                                                            (ModulePtr<Value>)(to - *module.arena)));
        auto castFrom = resolver.ref(resolver.emit<InstUnary>(source, 0, byteType, Value::Cast,
                                                              (ModulePtr<Value>)(from - *module.arena)));

        auto copyInst = resolver.create<InstNative>(source, 0, module.scalar.unit, NativeOp::CopyMemory);
        copyInst->args.push(module.arena, castTo);
        copyInst->args.push(module.arena, castFrom);
        copyInst->args.push(module.arena, bytes);
        resolver.append(copyInst);
    } else if(ownership.authoredSink) {
        // The authored `Sink` takes `(&to: a, ->from: a)`, and both conventions arrive as addresses,
        // so the two pointers this function was given are already the arguments it wants.
        TypePtr args[] = { type };
        auto match = matchInstance(module, module.coreClasses.sink, toBuffer(args));

        if(match && !(*module.arena)[match.instance]->functions.isEmpty()) {
            auto implementation = (*module.arena)[match.instance]->functions.get(*module.arena, 0);

            if(implementation && (*module.arena)[implementation]->gen) {
                implementation = instantiateFunction(module, implementation, toBuffer(match.args), source);
            }

            if(implementation) {
                (*module.arena)[implementation]->used = true;

                auto call = resolver.create<InstCall>(source, 0, module.scalar.unit, implementation);
                call->args.push(module.arena, (ModulePtr<Value>)(to - *module.arena));
                call->args.push(module.arena, (ModulePtr<Value>)(from - *module.arena));
                resolver.append(call);
            }
        }
    }

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    return pointer;
}

/*
 * Whether a place can be addressed with constant offsets.
 *
 * Lowering walks a projection path accumulating byte offsets read off each type it passes through,
 * and a generic aggregate has none: `Pair(a, b).second` sits at an offset that depends on what `a`
 * turned out to be, and a body compiled once for every `a` has no constant to use. What it needs is
 * the composite descriptor Implementation-Generics.md part 4 calls `reprOps`, whose "scoped
 * constructor-content projection" is exactly this.
 *
 * Until that exists, a body that projects into a generic type specializes instead. Depending on the
 * declaration's own offsets happening to be right - which they are whenever every field before the
 * projected one has a size independent of the arguments - would be an accident rather than a rule.
 */
static bool lowerablePlace(Module& module, Function& owner, const Place& place) {
    auto local = *module.arena;
    auto global = *module.types;
    auto projections = place.projections;
    if(projections.isEmpty()) return true;

    TypePtr type = nullptr;

    switch(place.root) {
        case PlaceRoot::Local:
            if(place.local >= owner.localCount()) return false;
            type = owner.localAt(local, place.local).type;
            break;
        case PlaceRoot::Global:
            type = local[place.global]->type;
            break;
        case PlaceRoot::Pointer:
            type = pointeeType(global, local[place.pointer]->type);
            break;
        case PlaceRoot::Borrow:
            type = ((BorrowType*)global[local[place.pointer]->type])->to;
            break;
    }

    for(auto projection: projections.contents(local)) {
        if(!type) return false;
        if(isGeneric(global, type)) return false;

        switch(projection.kind) {
            case ProjectionKind::Discriminant:
                type = module.scalar.int_;
                break;
            case ProjectionKind::Downcast:
                type = ((RecordType*)global[type])->constructors.get(global, projection.index).content;
                break;
            case ProjectionKind::Field:
                type = ((TupType*)global[type])->fields.get(global, projection.index).type;
                break;
            case ProjectionKind::Deref:
                type = pointeeType(global, type);
                break;
            case ProjectionKind::Index:
                return false;
        }
    }

    return true;
}

static bool lowerablePlaces(Module& module, Function& owner, Value& inst) {
    switch(inst.kind) {
        case Value::LoadPlace: return lowerablePlace(module, owner, ((InstLoadPlace&)inst).place);
        case Value::Init:
        case Value::Assign: return lowerablePlace(module, owner, ((InstInit&)inst).place);
        case Value::Borrow: return lowerablePlace(module, owner, ((InstBorrow&)inst).place);
        case Value::Move: return lowerablePlace(module, owner, ((InstMove&)inst).place);
        case Value::Address: return lowerablePlace(module, owner, ((InstAddress&)inst).place);
        case Value::Drop: return lowerablePlace(module, owner, ((InstDrop&)inst).place);
        default: return true;
    }
}

bool genericBodyLowerable(Module& module, ModulePtr<Function> function) {
    auto local = *module.arena;
    auto target = local[function];

    // A signature or an intrinsic has no body to emit; the first is not callable at all and the
    // second is generated at each call site, so neither is a candidate.
    if(target->signature || target->intrinsic) return false;

    for(auto blockPointer: target->blocks.contents(local)) {
        auto block = local[blockPointer];

        for(auto instruction: block->instructions.contents(local)) {
            auto& inst = *local[instruction];

            // A deferred call - one the body could not decide - has no environment of its own, and
            // supplying one means projecting this function's slots into the callee's.
            if(inst.kind == Value::GenCall && !((InstGenCall&)inst).env) return false;

            // An explicit copy of a value whose type the body cannot see needs the `Copy` witness,
            // which is a class constraint and so is on the same list as the two above.
            if(inst.kind == Value::Copy && isGeneric(*module.types, inst.type)) return false;

            // A projection into a generic aggregate. `Pair(a, b).second` sits at an offset that
            // depends on what `a` turned out to be, and a body compiled once for every `a` has no
            // constant to use - it needs the composite descriptor Implementation-Generics.md part 4
            // calls `reprOps`, whose "scoped constructor-content projection" is exactly this.
            //
            // Until that exists the offsets a generic body would compute are the declaration's,
            // which are only right when every field before the projected one has a size independent
            // of the type arguments. Rather than depend on that accident, such a body specializes.
            if(!lowerablePlaces(module, *target, inst)) return false;
        }
    }

    return true;
}

ModulePtr<Global> genEnvFor(Module& module, ModulePtr<Function> callee, Buffer<TypePtr> args,
                            LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;
    auto& program = module.program;

    auto env = functionGen(global, *local[callee]);
    if(!env) return nullptr;

    for(auto& existing: program.genEnvs) {
        if(existing.callee != callee) continue;
        if(sameTypes(toBuffer(existing.args), args)) return existing.env;
    }

    auto& schema = genSchemaOf(module, *env);
    auto slotCount = schema.slots.size();

    StringBuilder text;
    text << "genEnv$" << context.findName(local[callee]->name) << '(';
    describeTypes(context, global, args, text);
    text << ')';

    auto global_ = addAnonymousGlobal(module, context.addQualifiedName(text.pointer(), text.size(), 1), source);
    auto pointer = global_ - local;

    // Registered before the slots are filled, since building one of them can ask for an environment
    // again - a witness whose implementation is itself generic.
    program.genEnvs.push(InternedEnv { callee, Array<TypePtr>(), pointer });
    auto& entry = program.genEnvs[program.genEnvs.size() - 1];
    for(auto arg: args) entry.args.push(arg);

    TableBuilder table(module, GenEnvLayout::sizeFor(slotCount));
    global_->contents = table.bytes;

    auto ok = true;

    for(auto slot: schema.slots.contents(global)) {
        auto offset = GenEnvLayout::slotOffset(slot.index);

        switch(slot.kind) {
            case GenSlotKind::Type: {
                auto concrete = substituteType(module, slot.type, args, source);
                auto descriptor = typeDescFor(module, concrete, source);

                if(!descriptor) {
                    context.diagnostics.error("%@ cannot be passed to generic code - it is not a concrete type"_v,
                                              source, describeType(context, global, concrete));
                    ok = false;
                    break;
                }

                table.putGlobal(*global_, offset, descriptor);
                break;
            }

            // The three witness kinds. Each one is a separate constraint entry with its own
            // implementation, and none of them is derivable from a TypeDesc - knowing a type's size
            // grants nothing else, which is Implementation-Generics.md part 1's fifth invariant.
            case GenSlotKind::Class:
            case GenSlotKind::Property:
            case GenSlotKind::Function:
                context.diagnostics.error("%@ cannot be called generically yet - its %@ requirement needs a witness, which is not built yet"_v,
                                          source, context.findName(local[callee]->name),
                                          slot.kind == GenSlotKind::Class ? "class"_v
                                          : slot.kind == GenSlotKind::Property ? "field"_v : "function"_v);
                ok = false;
                break;
        }
    }

    return ok ? pointer : nullptr;
}

ModulePtr<Global> typeDescFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.typeDescs.get(U32(type))) return found.unwrap();

    auto global_ = addAnonymousGlobal(module, tableName(module, "typeDesc$"_v, type), source);
    auto pointer = global_ - *module.arena;

    // Registered before the lifecycle functions are generated, since generating one for a type
    // reachable from itself asks for this descriptor again.
    *program.typeDescs.add(U32(type)).value = pointer;

    auto ownership = ownershipOf(module, type);
    auto size = typeSize(*module.types, type);
    auto align = typeAlign(*module.types, type);

    TableBuilder table(module, TypeDescLayout::kSize_);
    table.putU32(TypeDescLayout::kSize, size);
    table.putU32(TypeDescLayout::kAlign, align);
    table.putU32(TypeDescLayout::kStride, align ? ((size + align - 1) & ~(align - 1)) : size);

    // Nothing selects a non-canonical Repr yet, and no type declares that it must keep its address,
    // so the only source of a stable-address requirement is a Repr variant - which is Milestone 8's.
    table.putU32(TypeDescLayout::kFlags, typeDescFlags(ownership, false));

    global_->contents = table.bytes;
    table.putType(*global_, TypeDescLayout::kLogicalType, type);

    // Every lifecycle slot holds a callable address, so erased code never has to test one - see
    // emptyTeardown. A type whose bytes are its whole relocation still gets a real moveInit, since
    // that one has a size to copy and is not a no-op.
    auto orEmpty = [&](ModulePtr<Function> implementation) {
        return implementation ? implementation : emptyTeardown(module, source);
    };

    table.putFunction(*global_, TypeDescLayout::kMoveInit, orEmpty(moveInitFor(module, type, source)));
    table.putFunction(*global_, TypeDescLayout::kReclaim,
                      orEmpty(teardownImplementation(module, type, Teardown::Reclaim, source)));
    table.putFunction(*global_, TypeDescLayout::kDrop,
                      orEmpty(teardownImplementation(module, type, Teardown::Drop, source)));

    return pointer;
}
