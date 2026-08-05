#include "witness.h"
#include "analyze.h"
#include "expr.h"
#include "generic.h"
#include "name.h"

/*
 * The two tables the typed IR reaches into, as tuple types.
 *
 * A field of one of these is the slot of the same number - the tuples below are the slot lists of
 * witness.h's numberings, written as types so that a place rooted in a table address can read one
 * the way any other aggregate is read. Nothing here says where a field *is*; that is the emitting
 * target's, and whether its answer for the tuple agrees with its answer for the slot list is what
 * checkTableTypes asserts on the native side, where both descriptions exist.
 */
TypePtr typeDescPlaceType(Module& module) {
    auto& context = module.context;
    auto word = module.scalar.int_;
    auto address = resolvePointerType(module, module.scalar.unit);

    Field fields[] = {
        Field { word, context.addUnqualifiedName("logicalType", 11) },
        Field { word, context.addUnqualifiedName("size", 4) },
        Field { word, context.addUnqualifiedName("align", 5) },
        Field { word, context.addUnqualifiedName("stride", 6) },
        Field { word, context.addUnqualifiedName("flags", 5) },
        Field { address, context.addUnqualifiedName("moveInit", 8) },
        Field { address, context.addUnqualifiedName("copyInit", 8) },
        Field { address, context.addUnqualifiedName("reclaim", 7) },
        Field { address, context.addUnqualifiedName("drop", 4) },
    };

    // Pinned, because these bytes have two descriptions and both are already written: erased code
    // reads the table by slot number through repr/table.h, and this tuple is how the typed IR reads
    // the same words. A target free to reorder the fields would make the second description disagree
    // with the first - which is what checkTableTypes in resolve/lower.cpp asserts it does not.
    auto tuple = resolveTupleType(module, { fields, 9 }, kNullLocation, TypeLayout::C);
    return (Type*)tuple - *module.types;
}

TypePtr funValueFieldType(Module& module, U16 field) {
    // The two words happen to have the same type, which is not a reason to answer without looking:
    // a projection index that is neither of them describes no word of a function value, and handing
    // it an address anyway would let a place nothing laid out reach lowering as a load at an offset.
    switch(field) {
        case FunValueLayout::kCode:
        case FunValueLayout::kEnv:
            return resolvePointerType(module, module.scalar.unit);

        // The header the target attached to the code word, where it attached one - see
        // FunValueLayout::kHeader. Typed, unlike the two words, because what it points at is a
        // layout this compiler both writes and reads.
        case FunValueLayout::kHeader:
            return resolvePointerType(module, closureHeaderPlaceType(module));
        default:
            return module.scalar.error;
    }
}

U16 classSuperclassSlot(GlobalBase global, GlobalPtr<TypeClass> typeClass, U16 index) {
    auto argCount = U16(global[global[typeClass]->gen]->types.size());
    auto methodCount = U16(global[typeClass]->functions.size());

    return ClassWitnessFields::super(argCount, methodCount, index);
}

TypePtr closureHeaderPlaceType(Module& module) {
    auto& context = module.context;
    auto address = resolvePointerType(module, module.scalar.unit);

    Field fields[] = {
        Field { address, context.addUnqualifiedName("drop", 4) },
        Field { address, context.addUnqualifiedName("reclaim", 7) },
    };

    // Pinned for the same reason as the descriptor, and more strictly: the code generator places
    // these two words at exactly this distance in front of an entry point.
    auto tuple = resolveTupleType(module, { fields, 2 }, kNullLocation, TypeLayout::C);
    return (Type*)tuple - *module.types;
}

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
    global_->anonymous = true;
    module.globalOrder.push(module.arena, global_ - *module.arena);
    return global_;
}

/*
 * The slots of one compiler-built table.
 *
 * Filled by slot number rather than in write order, because what goes in a slot becomes available in
 * whatever order the things it names get built - a descriptor's lifecycle glue is generated after
 * its numbers are known, and a witness's superclasses after its methods. So the list is sized up
 * front and written into, which also means an unset slot is a null of the right kind rather than a
 * gap.
 *
 * Nothing here is bytes. A slot names a function or holds a number, and turning either into storage
 * is the backend's - see TableCell.
 */
struct TableBuilder {
    TableBuilder(Module& module, Global& target, U16 slots): module(module), target(target) {
        target.isTable = true;
        target.table.reserve(module.arena, slots);

        for(U16 i = 0; i < slots; i++) target.table.push(module.arena, TableSlot {});
    }

    void put(U16 slot, TableSlot value) {
        // A slot outside the numbering is a compiler bug rather than a program error, and pushing
        // one would silently make the table longer than the shape its readers were compiled against.
        assertTrue(slot < target.table.size());
        target.table.set(*module.arena, slot, value);
    }

    void putU32(U16 slot, U32 value) {
        put(slot, TableSlot { TableCell::Int, TypeMetricKind::Size, value, nullptr, nullptr });
    }

    // How wide a type is, left as the question rather than the answer - see TableCell::Metric. This
    // is what keeps a descriptor free of any one target's numbers.
    void putMetric(U16 slot, TypePtr type, TypeMetricKind metric) {
        put(slot, TableSlot { TableCell::Metric, metric, U32(type), nullptr, nullptr });
    }

    // A null target writes nothing, leaving the zero slot the constructor put there: that is how
    // "this type has no drop" reaches the reader as an empty slot rather than as a missing entry.
    void putFunction(U16 slot, ModulePtr<Function> function) {
        if(!function) return;

        (*module.arena)[function]->used = true;
        put(slot, TableSlot { TableCell::Function, TypeMetricKind::Size, 0, function, nullptr });
    }

    // An interned type, as its region offset. Its own kind rather than an Int so that a dump can
    // name the type instead of printing the offset - see TableCell::Type.
    void putType(U16 slot, TypePtr type) {
        put(slot, TableSlot { TableCell::Type, TypeMetricKind::Size, U32(type), nullptr, nullptr });
    }

    void putClass(U16 slot, GlobalPtr<TypeClass> typeClass) {
        put(slot, TableSlot { TableCell::Class, TypeMetricKind::Size, U32(typeClass), nullptr, nullptr });
    }

    void putGlobal(U16 slot, ModulePtr<Global> global_) {
        if(!global_) return;

        (*module.arena)[global_]->used = true;
        put(slot, TableSlot { TableCell::Global, TypeMetricKind::Size, 0, nullptr, global_ });
    }

    Module& module;
    Global& target;
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

// Whether any member of `content` relocates by a call rather than by its bytes. Asked instead of
// sinkFor() wherever the answer is only being tested, since asking sinkFor() would *generate* the
// glue for every member of every constructor of every record the question is asked about.
static bool hasSinkingMember(Module& module, TypePtr content) {
    auto global = *module.types;
    if(!content) return false;

    // A fixed array's `n` members are one type, so one answer covers all of them - and an empty one
    // has no member to ask about, whatever its element would have said.
    if(global[content]->kind == Type::Array) {
        auto array = (ArrayType*)global[content];
        return array->length && !ownershipOf(module, array->content).trivialSink;
    }

    if(global[content]->kind != Type::Tup) return false;

    for(auto field: ((TupType*)global[content])->fields.contents(global)) {
        // A boxed member relocates as its pointer, whatever relocating its *target* would have
        // taken: the target does not move, so nothing it said about its own address stops applying.
        // This is what lets a self-referential type become TrivialSink by boxing the edge.
        if(field.boxed) continue;
        if(!ownershipOf(module, field.type).trivialSink) return true;
    }

    return false;
}

/*
 * The per-member half of a derived relocation: one call for each member whose bytes are not the
 * whole story, projected off the two bases.
 *
 * Both projections are addressed rather than loaded, because that is what the callee wants either
 * way - `fn sink(&to: a, ->from: a)` passes both of its conventions as addresses, and generated
 * glue takes two raw pointers.
 */
// One call, given the two places the member occupies in the destination and the source.
static void sinkMember(ExprResolver& resolver, Module& module, Place to, Place from,
                       ModulePtr<Function> implementation, LocationId source) {
    auto toMember = resolver.addressOf(to, source, 0);
    auto fromMember = resolver.addressOf(from, source, 0);

    auto call = resolver.create<InstCall>(source, 0, module.scalar.unit, implementation);
    call->args.push(module.arena, toMember);
    call->args.push(module.arena, fromMember);
    resolver.append(call);
}

/*
 * A fixed array's per-element half - Implementation-Containers.md §6.
 *
 * `n` elements at a stride, walked with the same helper the teardown uses so that the two agree
 * about when a walk is unrolled and when it is a loop. The two bases are stepped together, which is
 * why this cannot go through `eachFixedElement` twice: the loop it emits owns the counter, so the
 * source's element has to be computed inside the same body as the destination's.
 */
static void sinkFixedElements(ExprResolver& resolver, Module& module, Place to, Place from,
                              ArrayType& array, LocationId source) {
    auto implementation = sinkFor(module, array.content, source);
    if(!implementation) return;

    // `to`'s side is what the walk hands out and `from`'s is the same element of the other array,
    // which is what makes this one body under either shape the walk chooses.
    resolver.eachFixedElement(to, array.content, array.length, source,
                              [&](Place destination, ModulePtr<Value> index) {
        sinkMember(resolver, module, destination,
                   resolver.project(from, ProjectionKind::Index, 0, index), implementation, source);
    });
}

static void sinkMembers(ExprResolver& resolver, Module& module, Place to, Place from,
                        TypePtr content, LocationId source) {
    auto global = *module.types;

    if(content && global[content]->kind == Type::Array) {
        sinkFixedElements(resolver, module, to, from, *(ArrayType*)global[content], source);
        return;
    }

    if(!content || global[content]->kind != Type::Tup) return;

    U16 index = 0;

    for(auto field: ((TupType*)global[content])->fields.contents(global)) {
        // Skipped for the reason hasSinkingMember gives: the block copy above already moved the
        // pointer, and the target it names has not moved at all.
        if(field.boxed) { index++; continue; }

        if(auto implementation = sinkFor(module, field.type, source)) {
            sinkMember(resolver, module, resolver.project(to, ProjectionKind::Field, index),
                       resolver.project(from, ProjectionKind::Field, index), implementation, source);
        }

        index++;
    }
}

} // namespace

/*
 * moveInit.
 *
 * Implementation-Generics.md part 4 lists three answers - a block copy for TrivialSink, the
 * authored `Sink` where one exists, and unavailable - and what every caller wants is one shape: a
 * two-argument function taking the destination and the source as raw pointers, so that generic code
 * can call it without knowing either type or size. An authored `Sink` is already that shape, since
 * `(&to: a, ->from: a) -> {}` is two addresses and a unit result, so it is named directly rather
 * than wrapped in an adapter that would forward its two arguments and nothing else.
 *
 * The fourth case is the one the list does not name and the one an aggregate usually is: a type
 * that is *neither*, because it has a member that is neither. Relocating it is its bytes plus a
 * call per such member - the same structural recursion the derived teardown is - and the bytes come
 * first so that the members which do relocate bitwise, the discriminant of a multi-constructor
 * record among them, are carried by the one copy rather than by a projection each. What that copy
 * writes over the non-trivial members is written again by their sinks, which is sound because a
 * destination is uninitialized until a sink has filled it and no one reads it in between.
 *
 * The "unavailable" case is not represented in the descriptor at all, deliberately. Whether a body
 * may move a value of an unknown type is a question its *schema* answers, and it is answered before
 * any of this is reached; a descriptor that carried "you may not" would be inviting a second, later
 * check of something already settled. What is reported here instead is the concrete gap: a type
 * whose relocation this compiler cannot state, which used to produce a function with an empty body.
 */
ModulePtr<Function> moveInitFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.moveInitGlue.get(U32(type))) return found.unwrap();

    auto ownership = ownershipOf(module, type);
    // Nothing to relocate: a type that carries no information at all. Asked of the logical shape
    // rather than of a size, so that the answer is the same on every target - whether a `()` needs
    // moving is a fact about `()`, not about how wide it happens to be.
    if(isUnit(*module.types, type)) return nullptr;

    if(ownership.authoredSink) {
        auto implementation = instanceImplementation(module, module.coreClasses.sink, type, source);
        if(implementation) *program.moveInitGlue.add(U32(type)).value = implementation;
        return implementation;
    }

    auto global = *module.types;
    auto record = global[type]->kind == Type::Record ? (RecordType*)global[type] : nullptr;

    /*
     * A type that relocates by neither rule and has no members to recurse into. Every kind that
     * reaches this is one ownershipOf classifies conservatively because it is not constructible yet
     * - Ref, Region, Map - and the conservative answer is worth keeping: emitting the block copy
     * anyway would turn "adding one of these is a decision" into a silently wrong default.
     */
    if(!ownership.trivialSink && !record && global[type]->kind != Type::Tup &&
       global[type]->kind != Type::Array) {
        module.context.diagnostics.error(
            "%@ cannot be relocated: it is not TrivialSink and has no Sink instance"_v, source,
            describeType(module.context, global, type));
        return nullptr;
    }

    auto function = addAnonymousFunction(module, derivedName(module, "moveInit$"_v, type), source);
    auto pointer = function - *module.arena;

    // Registered before the body is built, so a type reachable from itself finds the entry rather
    // than generating glue forever - the same arrangement teardownGlueFor relies on.
    *program.moveInitGlue.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto pointerType = resolvePointerType(module, type);
    auto to = function->addArg(module, module.context.addQualifiedName("to", 2, 1), pointerType, source);
    auto from = function->addArg(module, module.context.addQualifiedName("from", 4, 1), pointerType, source);

    ExprResolver resolver(module.context, module, *function);

    // The bytes. copyMemory rather than a load and a store because the size is a constant here and
    // the type may be an aggregate with no register form at all.
    {
        auto bytes = resolver.ref(resolver.emit<InstTypeMetric>(source, 0, module.scalar.long_,
                                                                type, TypeMetricKind::Size));
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
    }

    if(!ownership.trivialSink) {
        auto toBase = Place::atPointer((ModulePtr<Value>)(to - *module.arena));
        auto fromBase = Place::atPointer((ModulePtr<Value>)(from - *module.arena));

        if(!record) {
            sinkMembers(resolver, module, toBase, fromBase, type, source);
        } else if(record->layout == RecordType::Single) {
            sinkMembers(resolver, module,
                        resolver.project(toBase, ProjectionKind::Downcast, 0),
                        resolver.project(fromBase, ProjectionKind::Downcast, 0),
                        record->constructors.get(global, 0).content, source);
        } else if(record->layout == RecordType::Multi) {
            /*
             * Each constructor carries a different payload, so which members need a call depends on
             * which one is present. Read off the *source*, whose discriminant is the one that was
             * true before the copy above made it true of both.
             *
             * A chain of tests rather than a jump table, for the reason teardownGlueFor gives: `je`
             * is the IR's only conditional. A constructor whose payload relocates bitwise is
             * skipped entirely, since the copy above has already moved it.
             */
            auto exit = resolver.addBlock();

            for(auto constructor: record->constructors.contents(global)) {
                auto content = constructor.content;
                if(!hasSinkingMember(module, content)) continue;

                auto discriminant = resolver.load(
                    resolver.project(fromBase, ProjectionKind::Discriminant, 0), source);

                auto index = resolver.makeInt(source, module.scalar.int_, constructor.index);
                auto matches = resolver.emit<InstCmp>(source, 0, module.scalar.bool_,
                                                      discriminant, index, CompareOp::Eq);

                auto sinks = resolver.addBlock();
                auto next = resolver.addBlock();
                resolver.terminate(resolver.emit<InstJe>(source, 0, module.scalar.unit,
                                                         resolver.ref(matches), sinks, next));

                resolver.current = sinks;
                sinkMembers(resolver, module,
                            resolver.project(toBase, ProjectionKind::Downcast, U16(constructor.index)),
                            resolver.project(fromBase, ProjectionKind::Downcast, U16(constructor.index)),
                            content, source);
                resolver.terminate(resolver.emit<InstJmp>(source, 0, module.scalar.unit, exit));

                resolver.current = next;
            }

            resolver.terminate(resolver.emit<InstJmp>(source, 0, module.scalar.unit, exit));
            resolver.current = exit;
        }
    }

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    return pointer;
}

/*
 * copyInit - Implementation-JS-Closure.md part 5.2, and the operation `codegen/js/README.md` gap 4
 * said was missing.
 *
 * Three answers, which is `moveInit`'s list with `Copy` where that one has `Sink`:
 *
 *  - **TrivialCopy**: the bytes, as a real function rather than a flag, because the caller is
 *    generic code that does not know the size. Each backend compiles that block copy its own way -
 *    a `memcpy` natively, a property-by-property duplicate on JS - which is the whole reason this is
 *    generated resolve IR rather than a descriptor operation each target implements.
 *  - **An authored `Copy`**: an adapter, and unlike `Sink` it needs one. `fn sink(&to: a, ->from: a)`
 *    is already two addresses and a unit result, so a slot can name it directly; `fn copy(from: a)
 *    -> a` *returns* the duplicate, and a slot has to be entered with the destination. So the
 *    adapter is the call plus the write, and it is one function per type rather than a shape at
 *    every erased site.
 *  - **Neither**: null, and that is the language rather than a gap. A concrete duplicate of a
 *    non-TrivialCopy type with no authored `Copy` does not exist either - `copyValue` emits a *move*
 *    for one - so an erased body only reaches this slot when its own context declared `Copy(a)` or
 *    `TrivialCopy(a)`, and in both of those the answer above is a real function. The constraint is
 *    reported where it is stated, during context construction, rather than at the write.
 *
 * No structural recursion, and that is the difference from moveInitFor worth stating: an aggregate
 * whose members are all TrivialCopy is itself TrivialCopy, and one whose members are not cannot be
 * duplicated at all without an authored `Copy` that says how. There is no fourth case where the
 * bytes plus a call per member is the answer.
 */
ModulePtr<Function> copyInitFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.copyInitGlue.get(U32(type))) return found.unwrap();

    // Nothing to duplicate, on the same terms as moveInitFor: a fact about `()` rather than about
    // how wide it happens to be on the target that asked.
    if(isUnit(*module.types, type)) return nullptr;

    auto ownership = ownershipOf(module, type);
    if(!ownership.trivialCopy && !ownership.authoredCopy) return nullptr;

    auto function = addAnonymousFunction(module, derivedName(module, "copyInit$"_v, type), source);
    auto pointer = function - *module.arena;

    // Registered before the body, so a type reachable from itself finds the entry rather than
    // generating glue forever - the same arrangement moveInitFor and teardownGlueFor rely on.
    *program.copyInitGlue.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto pointerType = resolvePointerType(module, type);
    auto to = function->addArg(module, module.context.addQualifiedName("to", 2, 1), pointerType, source);
    auto from = function->addArg(module, module.context.addQualifiedName("from", 4, 1), pointerType, source);

    ExprResolver resolver(module.context, module, *function);
    auto toValue = (ModulePtr<Value>)(to - *module.arena);
    auto fromValue = (ModulePtr<Value>)(from - *module.arena);

    if(ownership.authoredCopy) {
        auto implementation = instanceImplementation(module, module.coreClasses.copy, type, source);

        if(implementation) {
            /*
             * `*to = copy(from)`.
             *
             * The argument is the pointer rather than a load of it, because `from: a` of a memory
             * type is passed as the address of the caller's storage - which is exactly what this
             * frame was handed. The result is a value of that type, and the write is the ordinary
             * initialization of uninitialized storage that every constructor performs.
             */
            auto duplicate = resolver.create<InstCall>(source, 0, type, implementation);
            duplicate->args.push(module.arena, fromValue);
            resolver.append(duplicate);

            resolver.initialize(Place::atPointer(toValue), resolver.ref(duplicate), source);
        }

        resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
        return pointer;
    }

    /*
     * The bytes, through copyMemory for the reason moveInitFor gives: the size is a constant here
     * and the type may be an aggregate with no register form at all. What each backend makes of it
     * is its own business, and on JS that is genBlockCopy's structural duplicate rather than
     * anything that would alias.
     */
    auto bytes = resolver.ref(resolver.emit<InstTypeMetric>(source, 0, module.scalar.long_,
                                                            type, TypeMetricKind::Size));
    auto byteType = resolvePointerType(module, module.scalar.unit);

    auto castTo = resolver.ref(resolver.emit<InstUnary>(source, 0, byteType, Value::Cast, toValue));
    auto castFrom = resolver.ref(resolver.emit<InstUnary>(source, 0, byteType, Value::Cast, fromValue));

    auto copyInst = resolver.create<InstNative>(source, 0, module.scalar.unit, NativeOp::CopyMemory);
    copyInst->args.push(module.arena, castTo);
    copyInst->args.push(module.arena, castFrom);
    copyInst->args.push(module.arena, bytes);
    resolver.append(copyInst);

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    return pointer;
}

/*
 * What relocating a value of this type runs, or null when relocating it is a block copy.
 *
 * The distinction moveInitFor cannot make, because a descriptor slot has to name *some* function:
 * an already-resolved move of a concrete type emits the copy itself and needs to know only whether
 * there is a call to make instead.
 */
ModulePtr<Function> sinkFor(Module& module, TypePtr type, LocationId source) {
    if(!type || isGeneric(*module.types, type)) return nullptr;
    if(ownershipOf(module, type).trivialSink) return nullptr;

    return moveInitFor(module, type, source);
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
    auto global = *module.types;
    auto projections = place.projections;
    if(projections.isEmpty()) return true;

    auto count = projections.size();
    auto lowerable = true;

    walkPlace(module, owner, place, [&](const PlaceStep& step) {
        auto last = step.at + 1 == count;

        /*
         * A constrained field, which is the one projection whose owner is *meant* to be a type this
         * body cannot see - it is reached by calling the witness the caller passed rather than by
         * adding an offset, so the generic-owner test below does not apply to it.
         *
         * Only as the last step. A path continuing through a property would need the read's result
         * to be an address that the rest of the path could project into, and what a witness hands
         * back is a value in storage this frame provided; the address of that storage names a copy,
         * so a write through it would go nowhere. Such a body specializes, where the whole path
         * becomes ordinary field access.
         */
        if(step.kind == ProjectionKind::Property) {
            lowerable = last;
            return false;
        }

        /*
         * Every other step is an offset read off the owner's declaration, so an owner this body
         * cannot see the shape of has no offset to read.
         *
         * An `Index` is the exception the general rule already covers: one element of a `[T *n]` is
         * reachable from an erased body, because the step is the element's stride and a stride is a
         * TypeMetric the environment already carries. Declining it would force a specialization at
         * every call site that touched a fixed array; the generic check here still catches the case
         * that genuinely has no layout.
         */
        if(!step.owner || step.broken || isGeneric(global, step.owner)) {
            lowerable = false;
            return false;
        }

        return true;
    });

    return lowerable;
}

static bool lowerablePlaces(Module& module, Function& owner, const Value& inst) {
    auto ok = true;
    eachPlace(inst, [&](const Place& place) { ok = lowerablePlace(module, owner, place) && ok; });

    /*
     * And the places an aggregate's components are, which `eachPlace` does not report.
     *
     * It reports the value being built with *no* projection - the prefix every component shares,
     * which is what alias analysis wants and is exactly the wrong thing here: an empty path is
     * trivially lowerable, so a construction inside a generic body looked addressable while the
     * fields it writes were not. `Just(value)` was the shape - the erased body was accepted, its
     * payload store took the declaration's own offset of zero, and the discriminant written in
     * front of it was overwritten by the payload.
     *
     * Which is also why this is a rule about the *path* rather than a new gap: the offsets those
     * stores need are the composite descriptor's, and until `reprOps` exists such a body
     * specializes, exactly as it did when the components were separate stores.
     */
    if(inst.kind == Value::Aggregate) {
        auto& aggregate = (InstAggregate&)inst;

        eachWrittenComponent(*module.arena, module.arena, aggregate,
                             [&](Place place, ModulePtr<Value>, Size) {
            ok = lowerablePlace(module, owner, place) && ok;
        });
    }

    return ok;
}

/*
 * The erased entry point of one class method.
 *
 * A generic body that deferred a dispatch knows only the class *signature*, written over the class's
 * own type variables - so it calls with the shape that signature has, not the shape the instance's
 * implementation has. Those differ exactly where the signature is generic: an argument declared `a`
 * arrives as an address whatever `a` turned out to be, and a result declared `a` goes back through
 * caller storage for the same reason.
 *
 * The thunk is the adapter between the two. It takes that erased shape, reads the concrete values
 * out of it, calls the implementation the ordinary way - which expands an intrinsic exactly as any
 * call site does, so a specialized `<` on Int stays one `cmp` behind the pointer - and writes the
 * result back where the caller asked.
 */
static ModulePtr<Function> erasedThunkFor(Module& module, GlobalPtr<TypeClass> typeClass,
                                          const InstanceMatch& match, Buffer<TypePtr> classArgs,
                                          U16 index, LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;

    auto entry = global[typeClass]->functions.get(global, index);
    if(!entry.fun) return nullptr;

    auto signature = local[entry.fun];

    // The method index rather than only the name, because a class may declare one name at two
    // arities - `Num` declares both the binary and the unary `-` - and a thunk per method needs a
    // symbol per method. Everything downstream is keyed by name, so a collision here would silently
    // merge two functions rather than being reported.
    StringBuilder text;
    text << "thunk$" << context.findName(global[typeClass]->name) << '(';
    describeTypes(context, global, classArgs, text);
    text << ")." << context.findName(entry.name) << '#';
    text.appendValue(U32(index));

    auto function = addAnonymousFunction(module, builtName(context, text), source);
    auto pointer = function - local;
    function->returnType = module.scalar.unit;
    function->used = true;

    /*
     * The result the caller allocated storage for, when the signature returns something whose size
     * the caller could not know. Declared before the arguments so that the erased shape is the same
     * one an unspecialized generic function has - hidden storage first, then what was written.
     *
     * `isMemoryType` and not `isGeneric`, and the two differ in both directions. A signature
     * returning `&a` is generic and is nonetheless one pointer whatever `a` is; one returning a
     * concrete record is not generic and is nonetheless too big to come back in a register. The
     * call site's rule is the size one - lower.cpp reads `isMemoryType(callee->returnType)` off this
     * same signature - so this has to be the same question or the two disagree about the shape.
     *
     * `Index.get` is where they first did. `-> &v` gave a thunk with a hidden result parameter and a
     * call site that expected a returned pointer, so the arguments landed one position over and the
     * borrow came back as whatever had been in the result register.
     */
    auto concreteResult = substituteType(module, signature->returnType, classArgs, source);
    auto erasedResult = isMemoryType(global, signature->returnType);
    Arg* resultArg = nullptr;

    if(erasedResult) {
        resultArg = function->addArg(module, context.addQualifiedName("result", 6, 1),
                                     resolvePointerType(module, concreteResult), source);
    } else {
        function->returnType = concreteResult;
    }

    SmallArray<Arg*, 8> parameters;

    // One bit per parameter: whether the erased signature passes it by address. Sized before the
    // walk because the class function's arity is what it is, and set as each one is classified.
    IndexSet byAddress;
    byAddress.reset(signature->args.size());

    for(auto argPointer: signature->args.contents(local)) {
        auto declared = local[argPointer]->type;
        auto concrete = substituteType(module, declared, classArgs, source);
        auto erased = isGeneric(global, declared);

        byAddress.set(parameters.size(), erased);
        parameters.push(function->addArg(module, local[argPointer]->name,
                                         erased ? resolvePointerType(module, concrete) : concrete, source));
    }

    ExprResolver resolver(context, module, *function);
    ValueList args;

    for(Size i = 0; i < parameters.size(); i++) {
        auto value = (ModulePtr<Value>)(parameters[i] - local);
        args.push(byAddress[i] ? resolver.load(Place::atPointer(value), source) : value);
    }

    TypeList instanceArgs;
    for(auto arg: match.args) instanceArgs.push(arg);

    auto errors = context.diagnostics.errorCount();
    auto result = resolver.emitInstanceCall(module, match.instance, toBuffer(instanceArgs), index,
                                            toBuffer(args), source);

    if(context.diagnostics.errorCount() != errors) return nullptr;

    if(erasedResult) {
        // The storage arrived uninitialized, so this is an Init rather than an Assign: there is no
        // old value in it for anything to have to release.
        if(result) {
            resolver.initialize(Place::atPointer((ModulePtr<Value>)(resultArg - local)), result, source);
        }

        resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    } else {
        resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, result));
    }

    return pointer;
}

ModulePtr<Global> classWitnessFor(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                  LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto& program = module.program;

    for(auto& existing: program.classWitnesses) {
        if(existing.typeClass != typeClass) continue;
        if(sameTypes(toBuffer(existing.args), args)) return existing.witness;
    }

    for(auto arg: args) {
        if(isGeneric(global, arg)) return nullptr;
    }

    auto match = matchInstance(module, typeClass, args);
    if(!match) {
        StringBuilder types;
        describeTypes(context, global, args, types);

        context.diagnostics.error("no instance of %@ for (%@)"_v, source,
                                  context.findName(global[typeClass]->name), types.view());
        return nullptr;
    }

    auto classEnv = global[global[typeClass]->gen];
    auto methodCount = U16(global[typeClass]->functions.size());
    auto argCount = U16(args.length);
    auto superCount = U16(classEnv->classes.size());

    StringBuilder text;
    text << "witness$" << context.findName(global[typeClass]->name) << '(';
    describeTypes(context, global, args, text);
    text << ')';

    auto global_ = addAnonymousGlobal(module, builtName(context, text), source);
    auto pointer = global_ - *module.arena;

    // Registered before the thunks are generated, since one of them can ask for this witness again -
    // a class whose default implementation calls another method of the same class. The entry is
    // built whole and then pushed: generating a thunk can push another witness, and a reference into
    // a list that reallocates under it would be writing into freed storage.
    InternedWitness interned { typeClass, TypeList(), pointer };
    for(auto arg: args) interned.args.push(arg);

    auto entry = program.classWitnesses.size();
    program.classWitnesses.push(::move(interned));

    TableBuilder table(module, *global_,
                       ClassWitnessFields::countFor(argCount, methodCount, superCount));
    table.putClass(ClassWitnessFields::kClass, typeClass);
    table.putU32(ClassWitnessFields::kCounts, U32(argCount) | (U32(methodCount) << 16));
    table.putU32(ClassWitnessFields::kSuperCount, superCount);

    for(U16 i = 0; i < argCount; i++) {
        auto descriptor = typeDescFor(module, args[i], source);
        if(descriptor) table.putGlobal(ClassWitnessFields::kArgs + i, descriptor);
    }

    auto ok = true;

    for(U16 i = 0; i < methodCount; i++) {
        // A signature the instance does not implement leaves a null slot rather than failing the
        // whole witness: a body that never calls that method is unaffected, and one that does was
        // already rejected where the call was written.
        if(!(*module.arena)[match.instance]->functions.get(*module.arena, i)) continue;

        auto thunk = erasedThunkFor(module, typeClass, match, args, i, source);
        if(!thunk) {
            ok = false;
            continue;
        }

        table.putFunction(ClassWitnessFields::method(argCount, i), thunk);
    }

    /*
     * The superclasses, each a witness of its own for the types this one was selected at.
     *
     * Always present, never derived on demand: a body reaching one of these has already been
     * compiled to load it from this slot, and the instance it belongs to is not knowable there. That
     * every one of them exists is what checkSuperclasses settles at the declaration - an instance of
     * a class whose superclass has none was rejected long before anything asked for this table.
     */
    U16 superIndex = 0;

    for(auto constraint: classEnv->classes.contents(global)) {
        auto slot = superIndex++;
        if(!constraint.typeClass) continue;

        TypeList concrete;
        for(auto arg: constraint.args.contents(global)) {
            concrete.push(substituteType(module, arg, args, source));
        }

        auto superclass = classWitnessFor(module, constraint.typeClass, toBuffer(concrete), source);
        if(!superclass) {
            ok = false;
            continue;
        }

        table.putGlobal(ClassWitnessFields::super(argCount, methodCount, slot), superclass);
    }

    if(!ok) {
        /*
         * The entry has to go with the failure. Interning early is what lets this recurse, but it
         * also means a half-built table is sitting in the cache under a key that is still being
         * asked about: the next request would find it, skip the diagnostic this call just reported,
         * and take the table - whose failed method slot is a null the erased call would jump to.
         *
         * Removing it by the index it was pushed at rather than popping, because generating the
         * thunks pushed entries of their own and those are the successful ones. Nothing removes an
         * entry it did not push, so every index below this one is still what it was.
         */
        program.classWitnesses.remove(entry);
        return nullptr;
    }

    return pointer;
}

/*
 * Where one named field of a concrete owner sits, as the projections that reach it.
 *
 * The same two steps `projectField` and `resolveProperty` take, and here for the third time because
 * this is the third consumer: a single-constructor record *is* its content, reached through a
 * downcast that costs nothing, and the field is a position in the tuple behind it. False when the
 * owner has no such field, which is a caller that was never checked rather than a program error.
 */
static bool propertyPlace(Module& module, TypePtr owner, StringId field, Place& into, TypePtr& fieldType) {
    auto global = *module.types;
    auto content = owner;

    if(owner && global[owner]->kind == Type::Record) {
        auto record = (RecordType*)global[owner];
        if(record->layout != RecordType::Single || record->constructors.isEmpty()) return false;

        into.projections.push(module.arena, Projection { ProjectionKind::Downcast, 0, nullptr });
        content = record->constructors.get(global, 0).content;
    }

    if(!content || global[content]->kind != Type::Tup) return false;

    auto tuple = (TupType*)global[content];
    for(Size i = 0; i < tuple->fields.size(); i++) {
        auto entry = tuple->fields.get(global, i);
        if(entry.name != field) continue;

        into.projections.push(module.arena, Projection { ProjectionKind::Field, U16(i), nullptr });
        fieldType = entry.type;
        return true;
    }

    return false;
}

// The name both accessors are built under, which differs only in the verb. Generated names are not
// addressable in source, so all they have to be is unique and legible in a dump.
static StringId accessorName(Module& module, StringView verb, TypePtr owner, StringId field) {
    auto& context = module.context;

    StringBuilder text;
    text << verb << '$';
    describeType(context, *module.types, owner, text);
    text << '.' << context.findName(field);

    return builtName(context, text);
}

/*
 * `read(owner, out)` - the field's logical value, into storage the caller provided.
 *
 * Both parameters are addresses, which is the erased shape every witness operation has: the body
 * calling this was compiled once and does not know either type's size. What is *inside* is an
 * ordinary field read of a type this function can see, so the widen-and-mask a packed field needs
 * and the plain load an inline one needs are both just what lowering already does with the place.
 */
static ModulePtr<Function> propertyReadThunk(Module& module, TypePtr owner, StringId field,
                                             TypePtr fieldType, LocationId source) {
    auto& context = module.context;
    auto local = *module.arena;

    auto function = addAnonymousFunction(module, accessorName(module, "read"_v, owner, field), source);
    auto pointer = function - local;
    function->returnType = module.scalar.unit;
    function->used = true;

    auto ownerArg = function->addArg(module, context.addQualifiedName("owner", 5, 1),
                                     resolvePointerType(module, owner), source);
    auto outArg = function->addArg(module, context.addQualifiedName("out", 3, 1),
                                   resolvePointerType(module, fieldType), source);

    ExprResolver resolver(context, module, *function);

    auto place = Place::atPointer((ModulePtr<Value>)(ownerArg - local));
    TypePtr found = nullptr;
    if(!propertyPlace(module, owner, field, place, found)) return nullptr;

    // Init rather than Assign: the caller's storage arrived uninitialized, so there is no old value
    // in it for anything to have to release.
    resolver.initialize(Place::atPointer((ModulePtr<Value>)(outArg - local)),
                        resolver.load(place, source), source);
    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));

    return pointer;
}

/*
 * `set(owner, value)` - commit an owned replacement, releasing what was there.
 *
 * An Assign rather than an Init, which is the whole difference from the read side: the field held a
 * live value and whatever that value owed is owed at this point. The drop pass puts it in.
 */
static ModulePtr<Function> propertySetThunk(Module& module, TypePtr owner, StringId field,
                                            TypePtr fieldType, LocationId source) {
    auto& context = module.context;
    auto local = *module.arena;

    auto function = addAnonymousFunction(module, accessorName(module, "set"_v, owner, field), source);
    auto pointer = function - local;
    function->returnType = module.scalar.unit;
    function->used = true;

    auto ownerArg = function->addArg(module, context.addQualifiedName("owner", 5, 1),
                                     resolvePointerType(module, owner), source);
    auto valueArg = function->addArg(module, context.addQualifiedName("value", 5, 1),
                                     resolvePointerType(module, fieldType), source);

    ExprResolver resolver(context, module, *function);

    auto place = Place::atPointer((ModulePtr<Value>)(ownerArg - local));
    TypePtr found = nullptr;
    if(!propertyPlace(module, owner, field, place, found)) return nullptr;

    auto incoming = resolver.load(Place::atPointer((ModulePtr<Value>)(valueArg - local)), source);
    resolver.assign(place, incoming, source);
    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));

    return pointer;
}

ModulePtr<Global> propertyWitnessFor(Module& module, TypePtr owner, StringId field, TypePtr result,
                                     LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto& program = module.program;

    if(!owner || isGeneric(global, owner)) return nullptr;

    for(auto& existing: program.propertyWitnesses) {
        if(existing.owner == owner && existing.field == field) return existing.witness;
    }

    Place probe;
    TypePtr fieldType = nullptr;

    if(!propertyPlace(module, owner, field, probe, fieldType)) {
        context.diagnostics.error("%@ has no field %@"_v, source,
                                  describeType(context, global, owner), context.findName(field));
        return nullptr;
    }

    /*
     * The constraint promised a type and the owner has to have exactly it.
     *
     * Exactly rather than convertibly, for the reason Reject.Property.yana states: the body may
     * write back through this, and a conversion would commit into storage of a different shape.
     * Checked here as well as at the call site because this is what the *witness* would be built
     * from, and a mismatch reaching the table would be a silent reinterpretation.
     */
    if(result && !sameType(result, fieldType)) {
        context.diagnostics.error("field %@ of %@ is %@, but the constraint promised %@"_v, source,
                                  context.findName(field), describeType(context, global, owner),
                                  describeType(context, global, fieldType),
                                  describeType(context, global, result));
        return nullptr;
    }

    StringBuilder text;
    text << "property$";
    describeType(context, global, owner, text);
    text << '.' << context.findName(field);

    auto global_ = addAnonymousGlobal(module, builtName(context, text), source);
    auto pointer = global_ - *module.arena;

    // Registered before the accessors are generated, for the reason classWitnessFor gives: building
    // one can ask for another witness, and a reference into a list that reallocated under it would
    // be a write into freed storage.
    auto entry = program.propertyWitnesses.size();
    program.propertyWitnesses.push(InternedProperty {
        .owner = owner, .field = field, .witness = pointer,
    });

    TableBuilder table(module, *global_, PropertyWitnessFields::kCount);

    auto ownerDesc = typeDescFor(module, owner, source);
    auto fieldDesc = typeDescFor(module, fieldType, source);
    auto read = propertyReadThunk(module, owner, field, fieldType, source);
    auto set = propertySetThunk(module, owner, field, fieldType, source);

    if(!ownerDesc || !fieldDesc || !read || !set) {
        program.propertyWitnesses.remove(entry);
        return nullptr;
    }

    table.putGlobal(PropertyWitnessFields::kOwner, ownerDesc);
    table.putGlobal(PropertyWitnessFields::kField, fieldDesc);
    table.putFunction(PropertyWitnessFields::kRead, read);
    table.putFunction(PropertyWitnessFields::kSet, set);

    return pointer;
}

/*
 * Filling one call site's environment.
 *
 * For each slot of the callee's schema, the caller expresses that slot in its *own* terms - by
 * substituting the type arguments it is calling with - and then answers one question: is what came
 * out concrete, or is it one of my own type variables?
 *
 *  - concrete: store the address of an interned constant. This is part 9's static case, and it is
 *    the whole environment whenever the caller is not itself generic.
 *  - one of mine: copy my own slot across. This is the forwarded case, and it is what makes a chain
 *    of generic calls work - `middle` hands `smaller` the `Ord` witness it was handed itself, and
 *    nobody in the chain except the outermost concrete caller ever knows what the type is.
 *
 * A mix of the two is part 9's third case and needs no separate handling: the plan is per slot, so
 * a call that knows two of its four slots concretely simply has two of each.
 */
static bool fillEnvironment(Module& module, Function& caller, InstGenCall& call, GenEnv& calleeEnv,
                            Buffer<TypePtr> typeArgs) {
    auto global = *module.types;
    auto callerEnv = functionGen(global, caller);
    auto& schema = genSchemaOf(module, calleeEnv);
    auto ok = true;
    auto allConstant = true;

    call.fill.clear();

    for(auto slot: schema.slots.contents(global)) {
        GenSlotFill entry;

        if(slot.kind == GenSlotKind::Type) {
            auto expressed = substituteType(module, slot.type, typeArgs, call.source);

            if(isGeneric(global, expressed)) {
                entry.forwarded = callerEnv ? genTypeSlot(module, *callerEnv, expressed) : maxLimit<U16>;
                allConstant = false;
            } else {
                entry.constant = typeDescFor(module, expressed, call.source);
            }
        } else if(slot.kind == GenSlotKind::Class) {
            TypeList expressed;
            auto anyGeneric = false;

            for(auto arg: slot.args.contents(global)) {
                auto substituted = substituteType(module, arg, typeArgs, call.source);
                anyGeneric = anyGeneric || isGeneric(global, substituted);
                expressed.push(substituted);
            }

            if(anyGeneric) {
                Array<U32> supers;
                entry.forwarded = callerEnv
                    ? genWitnessPath(module, *callerEnv, slot.typeClass, toBuffer(expressed), supers)
                    : maxLimit<U16>;

                for(auto step: supers) entry.forwardedSupers.push(module.arena, step);
                allConstant = false;
            } else {
                entry.constant = classWitnessFor(module, slot.typeClass, toBuffer(expressed), call.source);
            }
        } else if(slot.kind == GenSlotKind::Property) {
            auto owner = substituteType(module, slot.type, typeArgs, call.source);

            if(isGeneric(global, owner)) {
                // The caller does not know the owner either, so it hands over the witness it was
                // handed - the same forwarding a class requirement uses, and what makes a chain of
                // generic functions each carrying `a.name` cost one witness rather than one per
                // frame.
                entry.forwarded = callerEnv
                    ? genPropertySlot(module, *callerEnv, owner, slot.name)
                    : maxLimit<U16>;
                allConstant = false;
            } else {
                auto result = substituteType(module, slot.result, typeArgs, call.source);
                entry.constant = propertyWitnessFor(module, owner, slot.name, result, call.source);
            }
        } else {
            // A function requirement, which needs a FunctionWitness - a witness kind that does not
            // exist yet. The call site falls back to specializing rather than being given a null
            // slot.
            ok = false;
        }

        if(!entry.constant && !entry.isForwarded()) ok = false;
        call.fill.push(module.arena, entry);
    }

    // Every slot concrete is the case worth interning: one constant shared by every call site that
    // supplies the same arguments, rather than a table assembled on each of their frames.
    if(ok && allConstant) {
        call.env = genEnvFor(module, call.callee, typeArgs, call.source);
        if(call.env) call.fill.clear();
    }

    return ok;
}

bool prepareGenericCalls(Program& program) {
    auto local = *program.arena;
    auto global = *program.types;
    auto ok = true;

    /*
     * Which generic bodies actually reach the backend.
     *
     * A call site marks the function it calls, but that function's own calls are only discovered by
     * looking at it - so `middle` being emitted is what makes `smaller` need emitting too. Run to a
     * fixpoint rather than in one pass, since the call graph is not in any particular order and a
     * function may be marked after it has already been walked.
     */
    for(auto changed = true; changed;) {
        changed = false;

        for(auto module: program.modules) {
            for(Size i = 0; i < module->functionOrder.size(); i++) {
                auto function = local[module->functionOrder.get(local, i)];
                if(function->gen && !function->genericallyUsed) continue;

                for(auto blockPointer: function->blocks.contents(local)) {
                    for(auto instruction: local[blockPointer]->instructions.contents(local)) {
                        auto& inst = *local[instruction];
                        if(inst.kind != Value::GenCall) continue;

                        auto& call = (InstGenCall&)inst;
                        if(call.typeClass || !call.callee) continue;

                        auto callee = local[call.callee];
                        if(!callee->gen || callee->genericallyUsed) continue;

                        callee->genericallyUsed = true;
                        callee->used = true;
                        changed = true;
                    }
                }
            }
        }
    }

    for(auto module: program.modules) {
        // Thunks and witnesses are appended while this runs, so the list is walked by index - the
        // same reason resolveModuleBodies and runProgramOwnership do.
        for(Size i = 0; i < module->functionOrder.size(); i++) {
            auto pointer = module->functionOrder.get(local, i);
            auto function = local[pointer];

            // Only a body that will actually be emitted. A generic function every call site
            // specialized has no machine code, so its deferred calls are decided by cloning.
            if(function->gen && !function->genericallyUsed) continue;

            for(auto blockPointer: function->blocks.contents(local)) {
                for(auto instruction: local[blockPointer]->instructions.contents(local)) {
                    auto& inst = *local[instruction];
                    if(inst.kind != Value::GenCall) continue;

                    auto& call = (InstGenCall&)inst;
                    if(call.env) continue;

                    TypeList typeArgs;
                    for(auto arg: call.typeArgs.contents(local)) typeArgs.push(arg);

                    if(call.typeClass) {
                        // A deferred class dispatch reads its witness out of the caller's own
                        // environment. Which slot that is could not be recorded when the call was
                        // emitted, because the context was still collecting requirements.
                        auto env = functionGen(global, *function);
                        Array<U32> supers;
                        call.classSlot = env
                            ? genWitnessPath(*module, *env, call.typeClass, toBuffer(typeArgs), supers)
                            : maxLimit<U16>;

                        call.classPath.clear();
                        for(auto step: supers) call.classPath.push(module->arena, step);

                        if(call.classSlot == maxLimit<U16>) {
                            module->context.diagnostics.error("internal: a deferred class call has no witness slot in %@"_v,
                                                              call.source, module->context.findName(function->name));
                            ok = false;
                        }

                        continue;
                    }

                    auto calleeEnv = functionGen(global, *local[call.callee]);
                    if(!calleeEnv) continue;

                    if(!fillEnvironment(*module, *function, call, *calleeEnv, toBuffer(typeArgs))) {
                        module->context.diagnostics.error("internal: %@ cannot be given an environment in %@"_v,
                                                          call.source,
                                                          module->context.findName(local[call.callee]->name),
                                                          module->context.findName(function->name));
                        ok = false;
                    }
                }
            }
        }
    }

    return ok;
}

/*
 * Whether a body can be emitted, and with it every generic body it calls.
 *
 * The recursion is the point rather than an optimization: emitting `middle` means emitting
 * `smaller`, because `middle`'s call to it is a real call that has to land somewhere. So a body is
 * lowerable only if the whole reachable set is, and one function that is not takes the entire chain
 * back to specialization.
 *
 * `visited` is what makes a recursive generic function terminate - it is on the stack, so assuming
 * it lowerable is exactly the right answer while deciding whether it is.
 */
static bool bodyLowerable(Module& module, ModulePtr<Function> function,
                          SmallArray<ModulePtr<Function>, 16>& visited, U32 depth) {
    auto local = *module.arena;
    auto global = *module.types;
    auto target = local[function];

    // A signature or an intrinsic has no body to emit; the first is not callable at all and the
    // second is generated at each call site, so neither is a candidate.
    if(target->signature || target->intrinsic) return false;
    if(!depth) return false;

    for(auto seen: visited) {
        if(seen == function) return true;
    }

    visited.push(function);

    // A function requirement needs a FunctionWitness, which does not exist yet. Checked on the
    // context rather than on the body, since it is the *contract* that cannot be supplied. Field
    // requirements are supplied - see propertyWitnessFor - and what limits them is what the *body*
    // does with them, which lowerablePlace below decides.
    if(auto env = functionGen(global, *target)) {
        if(env->functions.isNotEmpty()) return false;
    }

    for(auto blockPointer: target->blocks.contents(local)) {
        auto block = local[blockPointer];

        for(auto instruction: block->instructions.contents(local)) {
            auto& inst = *local[instruction];

            // An explicit copy of a value whose type the body cannot see needs the `Copy` witness,
            // which does not exist yet - a class witness holds methods, and `Copy` would have to be
            // reached as one rather than through the descriptor's lifecycle slots.
            if(inst.kind == Value::Copy && isGeneric(global, inst.type)) return false;

            // A projection into a generic aggregate. `Pair(a, b).second` sits at an offset that
            // depends on what `a` turned out to be, and a body compiled once for every `a` has no
            // constant to use - it needs the composite descriptor Implementation-Generics.md part 4
            // calls `reprOps`, whose "scoped constructor-content projection" is exactly this.
            //
            // Until that exists the offsets a generic body would compute are the declaration's,
            // which are only right when every field before the projected one has a size independent
            // of the type arguments. Rather than depend on that accident, such a body specializes.
            if(!lowerablePlaces(module, *target, inst)) return false;

            /*
             * A call through a function value whose *type* the body cannot see.
             *
             * `f: (a) -> a` is compiled once for every `a`, so the body passes `x` the way an erased
             * body passes everything - by address - while whatever the caller put in `f` is a
             * concrete function expecting whatever `a` turned out to be. Adapting between the two is
             * what a `FunctionWitness` is for: it carries the environment *and* the shape the
             * callable was compiled at, which a bare `{code, env}` does not.
             *
             * Until that exists such a body specializes, which is always available for a concrete
             * argument list - the same staging every other gap here uses.
             */
            if(inst.kind == Value::CallDyn && isGeneric(global, ((InstCallDyn&)inst).signature)) {
                return false;
            }

            /*
             * A continuation this body lifted out of itself - see Function::liftedFrom.
             *
             * It names this function's type variables and is specialized alongside it, which is the
             * one thing the erased form cannot do: there is one body here, and the lifted one would
             * have to be one body too, reading slots out of an environment nobody passes it. So a
             * generic function containing a lens call or a `for` loop specializes, which is always
             * available for a concrete argument list and is the same staging every other gap in this
             * walk uses.
             */
            if(inst.kind == Value::Symbol && ((InstSymbol&)inst).callee &&
               local[((InstSymbol&)inst).callee]->liftedFrom == function) {
                return false;
            }

            if(inst.kind != Value::GenCall) continue;

            auto& call = (InstGenCall&)inst;

            // A deferred class dispatch is supplied from this function's own environment, so what
            // it needs is that the requirement is on the context - which requireClass guarantees.
            if(call.typeClass) continue;

            // A call to another generic function means emitting that one too.
            if(!bodyLowerable(module, call.callee, visited, depth - 1)) return false;
        }
    }

    return true;
}

bool genericBodyLowerable(Module& module, ModulePtr<Function> function) {
    SmallArray<ModulePtr<Function>, 16> visited;
    return bodyLowerable(module, function, visited, 16);
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

    auto global_ = addAnonymousGlobal(module, builtName(context, text), source);
    auto pointer = global_ - local;

    // Registered before the slots are filled, since building one of them can ask for an environment
    // again - a witness whose implementation is itself generic. Built whole and then pushed, so that
    // a nested request growing the list cannot leave a reference into freed storage.
    InternedEnv interned { callee, TypeList(), pointer };
    for(auto arg: args) interned.args.push(arg);

    auto entry = program.genEnvs.size();
    program.genEnvs.push(::move(interned));

    TableBuilder table(module, *global_, GenEnvFields::countFor(slotCount));
    auto ok = true;

    for(auto slot: schema.slots.contents(global)) {
        auto cell = GenEnvFields::slot(slot.index);

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

                table.putGlobal(cell, descriptor);
                break;
            }

            case GenSlotKind::Class: {
                TypeList concrete;
                for(auto arg: slot.args.contents(global)) {
                    concrete.push(substituteType(module, arg, args, source));
                }

                auto witness = classWitnessFor(module, slot.typeClass, toBuffer(concrete), source);
                if(!witness) {
                    ok = false;
                    break;
                }

                table.putGlobal(cell, witness);
                break;
            }

            case GenSlotKind::Property: {
                auto owner = substituteType(module, slot.type, args, source);
                auto result = substituteType(module, slot.result, args, source);

                auto witness = propertyWitnessFor(module, owner, slot.name, result, source);
                if(!witness) {
                    ok = false;
                    break;
                }

                table.putGlobal(cell, witness);
                break;
            }

            // The one witness kind that does not exist yet. It is a separate constraint entry with
            // its own implementation and is not derivable from a TypeDesc - knowing a type's size
            // grants nothing else, which is part 1's fifth invariant.
            case GenSlotKind::Function:
                context.diagnostics.error("%@ cannot be called generically yet - its function requirement needs a witness, which is not built yet"_v,
                                          source, context.findName(local[callee]->name));
                ok = false;
                break;
        }
    }

    // Interned early so that a slot can ask for this environment again, and un-interned here for
    // the same reason classWitnessFor does: an environment with an unfilled slot is a table whose
    // reader would load a null, and a later request must be told what this one was told rather than
    // handed the wreckage silently. See the note there for why the index is still this entry's.
    if(!ok) {
        program.genEnvs.remove(entry);
        return nullptr;
    }

    return pointer;
}

ModulePtr<Global> typeDescFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.typeDescs.get(U32(type))) return found.unwrap();

    auto global_ = addAnonymousGlobal(module, derivedName(module, "typeDesc$"_v, type), source);
    auto pointer = global_ - *module.arena;

    // Registered before the lifecycle functions are generated, since generating one for a type
    // reachable from itself asks for this descriptor again.
    *program.typeDescs.add(U32(type)).value = pointer;

    auto ownership = ownershipOf(module, type);

    /*
     * The three measurements, as questions rather than answers.
     *
     * This is the last thing in resolve that used to know how wide a type was, and it did not have
     * to: `size`, `align` and `stride` are the emitting target's, and a descriptor whose numbers were
     * filled in here would be a native artifact that the JS backend then read as though it described
     * JS values. So the slot says *which measurement of which type* and whoever materializes the
     * table answers it - the same trade InstTypeMetric makes for the instruction form, for the same
     * reason.
     */
    TableBuilder table(module, *global_, TypeDescFields::kCount);
    table.putType(TypeDescFields::kLogicalType, type);
    table.putMetric(TypeDescFields::kSize, type, TypeMetricKind::Size);
    table.putMetric(TypeDescFields::kAlign, type, TypeMetricKind::Align);
    table.putMetric(TypeDescFields::kStride, type, TypeMetricKind::Stride);

    // Nothing selects a non-canonical Repr yet, and no type declares that it must keep its address,
    // so the only source of a stable-address requirement is a Repr variant - which is Milestone 8's.
    table.putU32(TypeDescFields::kFlags, typeDescFlags(ownership, false));

    // Every lifecycle slot holds a callable address, so erased code never has to test one - see
    // emptyTeardown. A type whose bytes are its whole relocation still gets a real moveInit, since
    // that one has a size to copy and is not a no-op.
    auto orEmpty = [&](ModulePtr<Function> implementation) {
        return implementation ? implementation : emptyTeardown(module, source);
    };

    /*
     * The *entry* rather than the implementation, which is the difference between what a slot can
     * be called with and what the teardown itself declares - see teardownEntry. A slot holds one
     * signature for every type that could fill it, and erased code has an address and nothing else.
     */
    table.putFunction(TypeDescFields::kMoveInit, orEmpty(moveInitFor(module, type, source)));
    table.putFunction(TypeDescFields::kCopyInit, orEmpty(copyInitFor(module, type, source)));
    table.putFunction(TypeDescFields::kReclaim,
                      orEmpty(teardownEntry(module, type, Teardown::Reclaim, source)));
    table.putFunction(TypeDescFields::kDrop,
                      orEmpty(teardownEntry(module, type, Teardown::Drop, source)));

    return pointer;
}

/*
 * The closure header.
 *
 * Not interned, and it is the one compiler-built table that is not: the others are keyed by what
 * they describe and shared by everything that describes it, while this one belongs to one lifted
 * function and is emitted at that function's address. Two lambdas capturing the same types are still
 * two lambdas with two entry points, and each needs its own bytes in front of its own.
 *
 * What goes *in* it is interned in the ordinary way, since both halves depend only on the
 * environment's type - which is what makes the header itself sixteen bytes of relocation rather than
 * anything generated.
 */
ModulePtr<Global> closureHeaderFor(Module& module, ModulePtr<Function> lambda, TypePtr envType,
                                   LocationId source) {
    auto function = (*module.arena)[lambda];
    if(function->closureHeader) return function->closureHeader;

    StringBuilder text;
    text << "closure$" << module.context.findName(function->name);
    auto name = builtName(module.context, text);

    auto global_ = addAnonymousGlobal(module, name, source);
    auto pointer = global_ - *module.arena;

    TableBuilder table(module, *global_, ClosureHeaderFields::kCount);
    global_->prefixOf = lambda;

    auto orEmpty = [&](ModulePtr<Function> implementation) {
        return implementation ? implementation : emptyTeardown(module, source);
    };

    // The entries rather than the implementations, for the reason typeDescFor gives: teardownFunValue
    // reaches these through an InstCallDyn with the environment's *address*, which is the same
    // uniform slot ABI a descriptor has and not the convention a teardown declares.
    table.putFunction(ClosureHeaderFields::kDrop,
                      orEmpty(teardownEntry(module, envType, Teardown::Drop, source)));

    // The frame-environment answer, which is also the safe one to start from: a header that never
    // reaches selectStorage releases the captures and leaves the storage alone, and storage nothing
    // decided about is storage in the frame.
    table.putFunction(ClosureHeaderFields::kReclaim,
                      orEmpty(teardownEntry(module, envType, Teardown::Reclaim, source)));

    function->closureHeader = pointer;
    return pointer;
}

/*
 * The heap environment's reclaim: the captures, and then the storage under them.
 *
 * A wrapper rather than a flag on the environment type's own reclaim, because the two callers want
 * different things from the same type - a closure whose environment is in the frame runs exactly the
 * inner one - and because which of them a lambda needs is settled at compile time. The shared
 * teardown a function value goes through never learns the difference: it calls what the header
 * names.
 */
ModulePtr<Function> closureReleaseFor(Module& module, TypePtr envType, LocationId source) {
    auto& program = module.program;
    if(auto found = program.closureRelease.get(U32(envType))) return found.unwrap();

    auto local = *module.arena;
    auto function = addAnonymousFunction(module, derivedName(module, "closureRelease$"_v, envType), source);
    auto pointer = function - local;
    *program.closureRelease.add(U32(envType)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto envPointer = resolvePointerType(module, envType);
    auto arg = function->addArg(module, module.context.addQualifiedName("env", 3, 1), envPointer, source);
    auto env = (ModulePtr<Value>)(arg - local);

    ExprResolver resolver(module.context, module, *function);

    if(auto reclaim = teardownImplementation(module, envType, Teardown::Reclaim, source)) {
        local[reclaim]->used = true;

        // The implementation takes its subject by `->`, and this wrapper was handed the address -
        // so the read is what bridges the slot ABI to the teardown's own. It is the same bridge
        // teardownEntry is, written inline because this function already exists and already has the
        // address; a second entry function here would be one more call for nothing.
        auto inner = resolver.create<InstCall>(source, 0, module.scalar.unit, reclaim);
        inner->args.push(module.arena, resolver.load(Place::atPointer(env), source));
        resolver.append(inner);
    }

    if(program.freeHeap) {
        auto free = local[program.freeHeap];
        free->used = true;

        // `freeHeap` is written over `%U8`, so the address is reinterpreted rather than converted -
        // both sides are one machine word and only what the program says it means differs.
        auto expected = free->args.isEmpty() ? envPointer : local[free->args.get(local, 0)]->type;
        auto address = sameType(expected, envPointer)
            ? env : resolver.ref(resolver.emit<InstUnary>(source, 0, expected, Value::Cast, env));

        auto release = resolver.create<InstCall>(source, 0, module.scalar.unit, program.freeHeap);
        release->args.push(module.arena, address);
        resolver.append(release);
    }

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    return pointer;
}

void setClosureRelease(Module& module, ModulePtr<Global> header, ModulePtr<Function> reclaim) {
    auto local = *module.arena;
    auto global_ = local[header];
    if(!reclaim) return;

    local[reclaim]->used = true;

    // The slot is overwritten rather than a second one appended, which is what a list of positions
    // makes obvious and a list of relocations did not: two entries for one slot would be two
    // addresses for one word, and which of them an emitter wrote would be whichever it saw last.
    global_->table.set(local, ClosureHeaderFields::kReclaim,
                       TableSlot { TableCell::Function, TypeMetricKind::Size, 0, reclaim, nullptr });
}
